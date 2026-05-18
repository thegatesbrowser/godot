/**************************************************************************/
/*  lockdown.cpp                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifdef LINUXBSD_ENABLED

#include "lockdown.h"

#include "seccomp_policy.h"

#include "core/error/error_macros.h"
#include "core/string/print_string.h"

#include "sandbox/linux/bpf_dsl/bpf_dsl.h"
#include "sandbox/linux/bpf_dsl/policy_compiler.h"
#include "sandbox/linux/seccomp-bpf/trap.h"
#include "sandbox/linux/system_headers/linux_filter.h"
#include "sandbox/linux/system_headers/linux_seccomp.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/landlock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <memory>
#include <vector>

namespace {

// Unbuffered write(2) to fd 2 — survives a SIGSYS or abort that follows.
void fputs_log(const String &p_msg) {
	const CharString utf8 = (p_msg + "\n").utf8();
	const ssize_t w = ::write(STDERR_FILENO, utf8.get_data(), (size_t)utf8.length());
	(void)w;
}

// glibc only exposes these from 2.36.
int landlock_create_ruleset(const struct landlock_ruleset_attr *attr,
		size_t size, uint32_t flags) {
	return (int)::syscall(__NR_landlock_create_ruleset, attr, size, flags);
}

int landlock_add_rule(int ruleset_fd, enum landlock_rule_type type,
		const void *rule_attr, uint32_t flags) {
	return (int)::syscall(__NR_landlock_add_rule, ruleset_fd, type, rule_attr, flags);
}

int landlock_restrict_self(int ruleset_fd, uint32_t flags) {
	return (int)::syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}

// landlock(7): older ABIs don't know REFER (ABI 2) or TRUNCATE (ABI 3);
// supported_fs_rights() masks the request against the kernel's set.
constexpr uint64_t kAllReadRights =
		LANDLOCK_ACCESS_FS_READ_FILE |
		LANDLOCK_ACCESS_FS_READ_DIR;

constexpr uint64_t kAllWriteRights =
		LANDLOCK_ACCESS_FS_WRITE_FILE |
		LANDLOCK_ACCESS_FS_REMOVE_DIR |
		LANDLOCK_ACCESS_FS_REMOVE_FILE |
		LANDLOCK_ACCESS_FS_MAKE_CHAR |
		LANDLOCK_ACCESS_FS_MAKE_DIR |
		LANDLOCK_ACCESS_FS_MAKE_REG |
		LANDLOCK_ACCESS_FS_MAKE_SOCK |
		LANDLOCK_ACCESS_FS_MAKE_FIFO |
		LANDLOCK_ACCESS_FS_MAKE_BLOCK |
		LANDLOCK_ACCESS_FS_MAKE_SYM |
#ifdef LANDLOCK_ACCESS_FS_REFER
		LANDLOCK_ACCESS_FS_REFER |
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
		LANDLOCK_ACCESS_FS_TRUNCATE |
#endif
		0;

uint64_t supported_fs_rights(int abi) {
	uint64_t mask = kAllReadRights | kAllWriteRights;
	if (abi < 2) {
#ifdef LANDLOCK_ACCESS_FS_REFER
		mask &= ~(uint64_t)LANDLOCK_ACCESS_FS_REFER;
#endif
	}
	if (abi < 3) {
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
		mask &= ~(uint64_t)LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
	}
	return mask;
}

// landlock_add_rule(2) rejects directory-only bits on regular files with
// EINVAL; mask the request to what the inode type accepts.
Error add_path_rule(int ruleset_fd, const String &p_path, uint64_t access) {
	const int fd = ::open(p_path.utf8().get_data(), O_PATH | O_CLOEXEC);
	if (fd < 0) {
		return OK;
	}
	struct stat st = {};
	if (::fstat(fd, &st) != 0) {
		::close(fd);
		return OK;
	}
	uint64_t masked = access;
	if (!S_ISDIR(st.st_mode)) {
		constexpr uint64_t kFileMask =
				LANDLOCK_ACCESS_FS_READ_FILE |
				LANDLOCK_ACCESS_FS_WRITE_FILE |
				LANDLOCK_ACCESS_FS_EXECUTE |
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
				LANDLOCK_ACCESS_FS_TRUNCATE |
#endif
				0;
		masked &= kFileMask;
	}
	if (masked == 0) {
		::close(fd);
		return OK;
	}
	struct landlock_path_beneath_attr attr = {};
	attr.allowed_access = masked;
	attr.parent_fd = fd;
	const int rc = landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &attr, 0);
	const int saved_errno = errno;
	::close(fd);
	if (rc < 0) {
		fputs_log(vformat(
				"SandboxLinux: landlock_add_rule(%s) failed errno=%d", p_path, saved_errno));
		return FAILED;
	}
	return OK;
}

Error apply_landlock(const String &p_rw_dir, const Vector<String> &p_rw_files,
		const Vector<String> &p_ro_files) {
	// Pre-5.13 kernels return -ENOSYS; skip landlock, seccomp + cap drop still apply.
	const int abi = landlock_create_ruleset(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 0) {
		return OK;
	}

	const uint64_t fs_rights = supported_fs_rights(abi);
	struct landlock_ruleset_attr ruleset = {};
	ruleset.handled_access_fs = fs_rights;

	const int ruleset_fd = landlock_create_ruleset(&ruleset, sizeof(ruleset), 0);
	if (ruleset_fd < 0) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux: landlock_create_ruleset failed errno=%d", (int)errno));
	}

	const uint64_t rw_rights = fs_rights;
	const uint64_t ro_rights = fs_rights & kAllReadRights;

	if (!p_rw_dir.is_empty()) {
		const Error err = add_path_rule(ruleset_fd, p_rw_dir, rw_rights);
		if (err != OK) {
			::close(ruleset_fd);
			return err;
		}
	}
	for (int i = 0; i < p_rw_files.size(); ++i) {
		const Error err = add_path_rule(ruleset_fd, p_rw_files[i], rw_rights);
		if (err != OK) {
			::close(ruleset_fd);
			return err;
		}
	}
	for (int i = 0; i < p_ro_files.size(); ++i) {
		const Error err = add_path_rule(ruleset_fd, p_ro_files[i], ro_rights);
		if (err != OK) {
			::close(ruleset_fd);
			return err;
		}
	}

	// Read-side roots the SandboxPolicy doesn't enumerate: shared libs, fonts,
	// nsswitch + /etc/vulkan ICD JSON, /proc + /sys for driver enumeration,
	// /etc/alsa + /proc/asound for ALSA fallback.
	static const char *kSystemReadRoots[] = {
		"/usr/lib",
		"/usr/lib64",
		"/lib",
		"/lib64",
		"/usr/share",
		"/etc/ld.so.cache",
		"/etc/ld.so.conf",
		"/etc/ld.so.conf.d",
		"/etc/fonts",
		"/etc/passwd",
		"/etc/nsswitch.conf",
		"/etc/host.conf",
		"/etc/hosts",
		"/etc/resolv.conf",
		"/etc/vulkan",
		"/etc/glvnd",
		"/etc/alsa",
		"/etc/pulse",
		"/proc/self",
		"/proc/cpuinfo",
		"/proc/meminfo",
		"/proc/sys/kernel",
		"/proc/asound",
		"/sys/devices",
		"/sys/class/drm",
		"/sys/class/sound",
		"/sys/dev/char",
	};
	for (const char *p : kSystemReadRoots) {
		add_path_rule(ruleset_fd, String::utf8(p), ro_rights);
	}

	add_path_rule(ruleset_fd, "/dev/dri", rw_rights);
	add_path_rule(ruleset_fd, "/dev/snd", rw_rights);
	add_path_rule(ruleset_fd, "/dev/shm", rw_rights);
	add_path_rule(ruleset_fd, "/tmp", rw_rights);
	add_path_rule(ruleset_fd, "/dev/null", rw_rights);
	add_path_rule(ruleset_fd, "/dev/zero", ro_rights);
	add_path_rule(ruleset_fd, "/dev/urandom", ro_rights);
	add_path_rule(ruleset_fd, "/dev/random", ro_rights);

	// PulseAudio's socket lives at $XDG_RUNTIME_DIR/pulse/, typically
	// /run/user/$UID/pulse/. Allow the runtime-dir tree so the renderer
	// can open the socket and read the auth cookie.
	{
		const char *xdg = ::getenv("XDG_RUNTIME_DIR");
		if (xdg != nullptr && xdg[0] != '\0') {
			add_path_rule(ruleset_fd, String::utf8(xdg), rw_rights);
		} else {
			add_path_rule(ruleset_fd, vformat("/run/user/%d", (int)::getuid()), rw_rights);
		}
	}

	if (landlock_restrict_self(ruleset_fd, 0) != 0) {
		const int saved_errno = errno;
		::close(ruleset_fd);
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux: landlock_restrict_self failed errno=%d", saved_errno));
	}
	::close(ruleset_fd);
	return OK;
}

Error drop_capabilities() {
	struct __user_cap_header_struct header = {};
	header.version = _LINUX_CAPABILITY_VERSION_3;
	header.pid = 0;
	struct __user_cap_data_struct data[2] = {};
	if (::syscall(__NR_capset, &header, data) != 0) {
		// EPERM == we already lack the privilege to drop further (the goal state).
		if (errno == EPERM) {
			return OK;
		}
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux: capset failed errno=%d", (int)errno));
	}
	return OK;
}

Error apply_seccomp() {
	TheGatesRendererPolicy policy;
	sandbox::bpf_dsl::PolicyCompiler compiler(&policy, sandbox::Trap::Registry());
	sandbox::CodeGen::Program program = compiler.Compile();

	sock_fprog prog = {};
	prog.len = (unsigned short)program.size();
	prog.filter = program.data();

	const long tsync_rc = ::syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
			SECCOMP_FILTER_FLAG_TSYNC, &prog);
	if (tsync_rc == 0) {
		return OK;
	}
	if (errno == ENOSYS || errno == EINVAL) {
		if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) == 0) {
			return OK;
		}
	}
	ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux: seccomp filter install failed errno=%d", (int)errno));
}

// Spectre v2 / v4 hardware barriers. Best-effort: older kernels return EINVAL,
// older CPUs return ENXIO — neither is fatal. Once set with PR_SPEC_DISABLE the
// flip is permanent on this thread group, so this runs once at lockdown.
#ifndef PR_SET_SPECULATION_CTRL
#define PR_SET_SPECULATION_CTRL 53
#endif
#ifndef PR_SPEC_STORE_BYPASS
#define PR_SPEC_STORE_BYPASS 0
#endif
#ifndef PR_SPEC_INDIRECT_BRANCH
#define PR_SPEC_INDIRECT_BRANCH 1
#endif
#ifndef PR_SPEC_DISABLE
#define PR_SPEC_DISABLE (1UL << 2)
#endif

void apply_speculation_ctrl() {
	if (::prctl(PR_SET_SPECULATION_CTRL, PR_SPEC_INDIRECT_BRANCH, PR_SPEC_DISABLE, 0, 0) != 0) {
		const int e = errno;
		if (e != EINVAL && e != ENXIO && e != ENODEV) {
			fputs_log(vformat("SandboxLinux: PR_SPEC_INDIRECT_BRANCH disable failed errno=%d", e));
		}
	}
	if (::prctl(PR_SET_SPECULATION_CTRL, PR_SPEC_STORE_BYPASS, PR_SPEC_DISABLE, 0, 0) != 0) {
		const int e = errno;
		if (e != EINVAL && e != ENXIO && e != ENODEV) {
			fputs_log(vformat("SandboxLinux: PR_SPEC_STORE_BYPASS disable failed errno=%d", e));
		}
	}
}

} // namespace

Error tg_apply_lockdown(const String &p_rw_dir,
		const Vector<String> &p_rw_files,
		const Vector<String> &p_ro_files) {
	if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux: PR_SET_NO_NEW_PRIVS failed errno=%d", (int)errno));
	}

	apply_speculation_ctrl();

	const Error landlock_err = apply_landlock(p_rw_dir, p_rw_files, p_ro_files);
	if (landlock_err != OK) {
		return landlock_err;
	}

	const Error cap_err = drop_capabilities();
	if (cap_err != OK) {
		return cap_err;
	}

	return apply_seccomp();
}

#endif // LINUXBSD_ENABLED
