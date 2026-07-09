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

int g_landlock_abi = 0;

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

// Read-side system roots the SandboxPolicy doesn't enumerate: shared libs,
// fonts, /etc system config (loader cache, nsswitch, hosts, Vulkan + GLVND
// ICD JSON, ALSA), /proc + /sys for hardware enumeration. /tmp/.X11-unix
// is ro so the inherited X11 FD's auth cookie can be re-read on draw.
constexpr const char *kSystemReadPaths[] = {
	"/usr",
	"/lib",
	"/lib64",
	"/etc",
	"/proc/cpuinfo",
	"/proc/meminfo",
	"/proc/sys/kernel",
	"/proc/asound",
	"/sys/dev/char",
	"/tmp/.X11-unix",
};

// /sys/class and /sys/devices enumerated, not granted as trees, so the
// hardware-fingerprint paths (/sys/class/dmi -> .../devices/virtual/dmi/id
// for board serial + product UUID, /sys/class/net -> .../net/<iface> for
// the persistent NIC MAC) stay unreachable.
constexpr const char *kSysReadPaths[] = {
	"/sys/class/drm",
	"/sys/class/input",
	"/sys/class/sound",
	"/sys/class/dma_heap",
	"/sys/class/iommu",
	"/sys/class/devfreq",
	"/sys/class/hidraw",
	"/sys/devices/pci0000:00",
	"/sys/devices/system/cpu",
	"/sys/devices/system/node",
	"/sys/devices/virtual/drm",
	"/sys/devices/virtual/input",
	"/sys/devices/virtual/dma_heap",
	"/sys/devices/virtual/sound",
	"/sys/devices/platform",
};

// /proc/self enumerated file-by-file, not granted as a tree, so /proc/self/net
// (i.e. the host's TCP/UDP socket tables via the /proc/net -> self/net
// symlink) stays unreachable.
constexpr const char *kProcSelfReadPaths[] = {
	"/proc/self/maps",
	"/proc/self/auxv",
	"/proc/self/status",
	"/proc/self/cmdline",
	"/proc/self/comm",
	"/proc/self/exe",
	"/proc/self/cwd",
	"/proc/self/stat",
	"/proc/self/statm",
	"/proc/self/limits",
	"/proc/self/cgroup",
	"/proc/self/setgroups",
	"/proc/self/mountinfo",
	"/proc/self/fd",
	"/proc/self/fdinfo",
	"/proc/self/task",
};

constexpr const char *kDevRwPaths[] = {
	"/dev/dri",
	"/dev/snd",
	"/dev/shm",
	"/tmp",
	"/dev/null",
};

constexpr const char *kDevReadPaths[] = {
	"/dev/zero",
	"/dev/urandom",
	"/dev/random",
};

Error add_static_paths(int p_ruleset_fd, const char *const *p_paths, size_t p_count, uint64_t p_rights) {
	for (size_t i = 0; i < p_count; ++i) {
		const Error err = add_path_rule(p_ruleset_fd, String::utf8(p_paths[i]), p_rights);
		if (err != OK) {
			return err;
		}
	}
	return OK;
}

// $XDG_RUNTIME_DIR carries Wayland, X11, PulseAudio, PipeWire — and a pile
// of secrets the renderer must not touch (SSH agent, session D-Bus, GNOME
// Keyring, systemd journal). Enumerate the specific graphics + audio
// sockets instead of granting the whole tree.
//
// Audio gated on policy. Linux can't separate mic from output at the socket
// layer, so passing false for either allow_audio or allow_microphone
// denies all audio.
void add_runtime_dir_paths(int p_ruleset_fd, uint64_t p_rw_rights, bool p_allow_audio, bool p_allow_microphone) {
	const char *xdg = ::getenv("XDG_RUNTIME_DIR");
	const String runtime_dir = (xdg != nullptr && xdg[0] != '\0')
			? String::utf8(xdg)
			: vformat("/run/user/%d", (int)::getuid());

	for (int i = 0; i < 4; ++i) {
		add_path_rule(p_ruleset_fd, vformat("%s/wayland-%d", runtime_dir, i), p_rw_rights);
		add_path_rule(p_ruleset_fd, vformat("%s/wayland-%d.lock", runtime_dir, i), p_rw_rights);
	}
	if (p_allow_audio && p_allow_microphone) {
		add_path_rule(p_ruleset_fd, runtime_dir + "/pulse", p_rw_rights);
		add_path_rule(p_ruleset_fd, runtime_dir + "/pipewire-0", p_rw_rights);
		add_path_rule(p_ruleset_fd, runtime_dir + "/pipewire-0.lock", p_rw_rights);
	}
}

// NVIDIA's proprietary driver reopens its device nodes at allocation time,
// not only at init, so post-lockdown vkAllocateMemory fails as
// VK_ERROR_OUT_OF_DEVICE_MEMORY without these. Chromium's GPU sandbox
// grants the same set.
void add_nvidia_dev_paths(int p_ruleset_fd, uint64_t p_rw_rights, uint64_t p_ro_rights) {
	constexpr const char *kNvidiaRwPaths[] = {
		"/dev/nvidiactl",
		"/dev/nvidia-modeset",
		"/dev/nvidia-uvm",
		"/dev/nvidia-uvm-tools",
	};
	for (size_t i = 0; i < sizeof(kNvidiaRwPaths) / sizeof(*kNvidiaRwPaths); ++i) {
		add_path_rule(p_ruleset_fd, String::utf8(kNvidiaRwPaths[i]), p_rw_rights);
	}
	for (int i = 0; i < 16; ++i) {
		add_path_rule(p_ruleset_fd, vformat("/dev/nvidia%d", i), p_rw_rights);
	}
	add_path_rule(p_ruleset_fd, "/dev/nvidia-caps", p_ro_rights);
	add_path_rule(p_ruleset_fd, "/proc/driver/nvidia", p_ro_rights);
}

// Fontconfig roots beyond /usr and /etc: Flatpak's host-font mounts and the
// user's own font dirs + fontconfig cache.
void add_font_paths(int p_ruleset_fd, uint64_t p_ro_rights) {
	constexpr const char *kHostFontPaths[] = {
		"/run/host/fonts",
		"/run/host/fonts-cache",
		"/run/host/local-fonts",
		"/run/host/user-fonts",
		"/run/host/user-fonts-cache",
		"/app/share/fonts",
	};
	for (size_t i = 0; i < sizeof(kHostFontPaths) / sizeof(*kHostFontPaths); ++i) {
		add_path_rule(p_ruleset_fd, String::utf8(kHostFontPaths[i]), p_ro_rights);
	}

	const char *home_env = ::getenv("HOME");
	if (home_env == nullptr || home_env[0] == '\0') {
		return;
	}
	const String home = String::utf8(home_env);
	const char *xdg_data = ::getenv("XDG_DATA_HOME");
	const String data_home = (xdg_data != nullptr && xdg_data[0] != '\0')
			? String::utf8(xdg_data)
			: home + "/.local/share";
	const char *xdg_cache = ::getenv("XDG_CACHE_HOME");
	const String cache_home = (xdg_cache != nullptr && xdg_cache[0] != '\0')
			? String::utf8(xdg_cache)
			: home + "/.cache";

	add_path_rule(p_ruleset_fd, home + "/.fonts", p_ro_rights);
	add_path_rule(p_ruleset_fd, data_home + "/fonts", p_ro_rights);
	add_path_rule(p_ruleset_fd, cache_home + "/fontconfig", p_ro_rights);
}

Error add_policy_paths(int p_ruleset_fd, const String &p_rw_dir,
		const Vector<String> &p_rw_files, const Vector<String> &p_ro_files,
		uint64_t p_rw_rights, uint64_t p_ro_rights) {
	if (!p_rw_dir.is_empty()) {
		const Error err = add_path_rule(p_ruleset_fd, p_rw_dir, p_rw_rights);
		if (err != OK) {
			return err;
		}
	}
	for (int i = 0; i < p_rw_files.size(); ++i) {
		const Error err = add_path_rule(p_ruleset_fd, p_rw_files[i], p_rw_rights);
		if (err != OK) {
			return err;
		}
	}
	for (int i = 0; i < p_ro_files.size(); ++i) {
		const Error err = add_path_rule(p_ruleset_fd, p_ro_files[i], p_ro_rights);
		if (err != OK) {
			return err;
		}
	}
	return OK;
}

Error apply_landlock(const String &p_rw_dir, const Vector<String> &p_rw_files,
		const Vector<String> &p_ro_files, bool p_allow_audio, bool p_allow_microphone) {
	// Pre-5.13 kernels return -ENOSYS; skip landlock, seccomp + cap drop still apply.
	const int abi = landlock_create_ruleset(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 0) {
		return OK;
	}
	g_landlock_abi = abi;

	const uint64_t fs_rights = supported_fs_rights(abi);
	struct landlock_ruleset_attr ruleset = {};
	ruleset.handled_access_fs = fs_rights;

	const int ruleset_fd = landlock_create_ruleset(&ruleset, sizeof(ruleset), 0);
	if (ruleset_fd < 0) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux: landlock_create_ruleset failed errno=%d", (int)errno));
	}

	const uint64_t rw_rights = fs_rights;
	const uint64_t ro_rights = fs_rights & kAllReadRights;

	Error err = add_policy_paths(ruleset_fd, p_rw_dir, p_rw_files, p_ro_files, rw_rights, ro_rights);
	if (err == OK) {
		err = add_static_paths(ruleset_fd, kSystemReadPaths,
				sizeof(kSystemReadPaths) / sizeof(*kSystemReadPaths), ro_rights);
	}
	if (err == OK) {
		err = add_static_paths(ruleset_fd, kSysReadPaths,
				sizeof(kSysReadPaths) / sizeof(*kSysReadPaths), ro_rights);
	}
	if (err == OK) {
		err = add_static_paths(ruleset_fd, kProcSelfReadPaths,
				sizeof(kProcSelfReadPaths) / sizeof(*kProcSelfReadPaths), ro_rights);
	}
	if (err == OK) {
		err = add_static_paths(ruleset_fd, kDevRwPaths,
				sizeof(kDevRwPaths) / sizeof(*kDevRwPaths), rw_rights);
	}
	if (err == OK) {
		err = add_static_paths(ruleset_fd, kDevReadPaths,
				sizeof(kDevReadPaths) / sizeof(*kDevReadPaths), ro_rights);
	}
	if (err != OK) {
		::close(ruleset_fd);
		return err;
	}

	add_runtime_dir_paths(ruleset_fd, rw_rights, p_allow_audio, p_allow_microphone);
	add_nvidia_dev_paths(ruleset_fd, rw_rights, ro_rights);
	add_font_paths(ruleset_fd, ro_rights);

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

int tg_lockdown_landlock_abi() {
	return g_landlock_abi;
}

Error tg_apply_lockdown(const String &p_rw_dir,
		const Vector<String> &p_rw_files,
		const Vector<String> &p_ro_files,
		bool p_allow_audio,
		bool p_allow_microphone) {
	if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux: PR_SET_NO_NEW_PRIVS failed errno=%d", (int)errno));
	}

	apply_speculation_ctrl();

	const Error landlock_err = apply_landlock(p_rw_dir, p_rw_files, p_ro_files, p_allow_audio, p_allow_microphone);
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
