/**************************************************************************/
/*  sandbox_linux.cpp                                                     */
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

#include "sandbox_linux.h"

#include "../sandbox_policy.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/string/print_string.h"

#ifdef LINUXBSD_ENABLED

#include "lockdown.h"
#include "signature_verify.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

void write_broker_policy_json(const String &p_log_path, const String &p_executable, pid_t p_pid,
		const Ref<SandboxPolicy> &p_policy) {
	Dictionary policy = p_policy->to_dict();
	policy["executable"] = p_executable;
	policy["pid"] = (int64_t)p_pid;
	policy["integrity_target"] = "untrusted";
	policy["userns_used"] = false;
	policy["landlock_requested"] = !p_policy->get_rw_dir().is_empty();
	policy["seccomp_policy"] = "TheGatesRendererPolicy";

	const String json_path = p_log_path.get_base_dir().path_join("broker_policy.json");
	Ref<FileAccess> file = FileAccess::open(json_path, FileAccess::WRITE);
	if (file.is_null()) {
		ERR_PRINT(vformat("SandboxLinux: cannot open %s for write", json_path));
		return;
	}
	file->store_string(JSON::stringify(policy, "\t", false));
}

void child_exec_after_log_dup(const char *log_path, char *const argv[], char *const envp[]) {
	if (log_path != nullptr) {
		const int log_fd = ::open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (log_fd < 0) {
			static const char msg[] = "tg_spawn: open(log_path) failed\n";
			const ssize_t w = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
			(void)w;
			::_exit(127);
		}
		if (::dup2(log_fd, STDOUT_FILENO) < 0 || ::dup2(log_fd, STDERR_FILENO) < 0) {
			::_exit(127);
		}
		if (log_fd != STDOUT_FILENO && log_fd != STDERR_FILENO) {
			::close(log_fd);
		}
	}
	::execve(argv[0], argv, envp);
	static const char msg[] = "tg_spawn: execve failed\n";
	const ssize_t w = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
	(void)w;
	::_exit(127);
}

} // namespace

#endif // LINUXBSD_ENABLED

Dictionary SandboxLinux::spawn_target(const Ref<SandboxPolicy> &p_policy,
		const String &p_executable, const Vector<String> &p_arguments) {
	Dictionary result;
#ifdef LINUXBSD_ENABLED
	ERR_FAIL_COND_V_MSG(p_policy.is_null(), result,
			"SandboxLinux::spawn_target requires a non-null SandboxPolicy");
	ERR_FAIL_COND_V_MSG(is_target(), result,
			"SandboxLinux::spawn_target called from a sandbox target process");

	const CharString exe_cs = p_executable.utf8();
	Vector<CharString> arg_storage;
	arg_storage.push_back(exe_cs);
	for (int i = 0; i < p_arguments.size(); ++i) {
		arg_storage.push_back(p_arguments[i].utf8());
	}
	Vector<char *> argv;
	for (int i = 0; i < arg_storage.size(); ++i) {
		argv.push_back(const_cast<char *>(arg_storage[i].get_data()));
	}
	argv.push_back(nullptr);

	// Policy crosses execve as TG_SANDBOX_* env vars; renderer's lower_token
	// reads them back. Keep names + protocol in sync with notes/Sandboxing/Linux Backend.md.
	Vector<CharString> env_owned;
	if (!p_policy->get_rw_dir().is_empty()) {
		env_owned.push_back(("TG_SANDBOX_RW_DIR=" + p_policy->get_rw_dir()).utf8());
	}
	const PackedStringArray rw_files = p_policy->get_rw_files();
	if (rw_files.size() > 0) {
		env_owned.push_back(("TG_SANDBOX_RW_FILES=" + String("|").join(rw_files)).utf8());
	}
	const PackedStringArray ro_files = p_policy->get_ro_files();
	if (ro_files.size() > 0) {
		env_owned.push_back(("TG_SANDBOX_RO_FILES=" + String("|").join(ro_files)).utf8());
	}

	int env_count = 0;
	for (char **e = environ; *e != nullptr; ++e) {
		env_count++;
	}
	Vector<char *> envp;
	envp.resize(env_count + env_owned.size() + 1);
	for (int i = 0; i < env_count; ++i) {
		envp.write[i] = environ[i];
	}
	for (int i = 0; i < env_owned.size(); ++i) {
		envp.write[env_count + i] = const_cast<char *>(env_owned[i].get_data());
	}
	envp.write[env_count + env_owned.size()] = nullptr;

	ERR_FAIL_COND_V_MSG(target_pid != 0 && is_target_running(), result,
			vformat("SandboxLinux::spawn_target: previous target pid=%d still running; call kill_target() first",
					(int)target_pid));
	if (target_pidfd >= 0) {
		::close(target_pidfd);
		target_pidfd = -1;
	}
	target_pid = 0;

	const CharString log_cs = p_policy->get_child_stdout_log_path().utf8();
	const char *log_path = log_cs.length() > 0 ? log_cs.get_data() : nullptr;

	const pid_t pid = ::fork();
	if (pid < 0) {
		ERR_FAIL_V_MSG(result, vformat("SandboxLinux::spawn_target: fork failed errno=%d", (int)errno));
	}
	if (pid == 0) {
		child_exec_after_log_dup(log_path, argv.ptrw(), envp.ptrw());
	}

	target_pid = (int64_t)pid;
	target_pidfd = (int)::syscall(__NR_pidfd_open, pid, 0);
	if (target_pidfd < 0) {
		print_line(vformat(
				"SandboxLinux::spawn_target: pidfd_open failed errno=%d (falling back to pid)", (int)errno));
	}
	result["pid"] = target_pid;

	if (log_path != nullptr) {
		write_broker_policy_json(p_policy->get_child_stdout_log_path(), p_executable, pid, p_policy);
	}
#else
	(void)p_policy;
	(void)p_executable;
	(void)p_arguments;
#endif
	return result;
}

SandboxLinux::~SandboxLinux() {
#ifdef LINUXBSD_ENABLED
	if (target_pidfd >= 0) {
		::close(target_pidfd);
		target_pidfd = -1;
	}
#endif
}

void SandboxLinux::apply_renderer_acl(const String &p_path) {
	// Same-uid + landlock handle isolation on Linux; no pre-spawn DACL stamp.
	(void)p_path;
}

Error SandboxLinux::_verify_binary_impl(const String &p_path) {
#ifdef LINUXBSD_ENABLED
	const char *force = ::getenv("TG_SIGNATURE_FORCE_FAIL");
	if (force != nullptr && force[0] == '1') {
		ERR_FAIL_V_MSG(ERR_UNAUTHORIZED,
				"SandboxLinux::verify_binary forced to fail by TG_SIGNATURE_FORCE_FAIL=1");
	}

#ifdef TG_SIGNATURE_PIN
	return tg_verify_renderer_binary(p_path, String(TG_SIGNATURE_PIN));
#else
	return tg_verify_renderer_binary(p_path, String());
#endif
#else
	(void)p_path;
	return OK;
#endif
}

bool SandboxLinux::is_target_running() const {
#ifdef LINUXBSD_ENABLED
	if (target_pid == 0) {
		return false;
	}
	// pidfd avoids PID-reuse races (poll readable once the process exits);
	// kill(0) is the < 5.3 fallback.
	if (target_pidfd >= 0) {
		struct pollfd pfd = { target_pidfd, POLLIN, 0 };
		const int rc = ::poll(&pfd, 1, 0);
		if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP)) != 0) {
			return false;
		}
		if (rc == 0) {
			return true;
		}
	}
	return ::kill((pid_t)target_pid, 0) == 0;
#else
	return false;
#endif
}

Error SandboxLinux::kill_target() {
#ifdef LINUXBSD_ENABLED
	if (target_pid == 0) {
		return ERR_DOES_NOT_EXIST;
	}
	const pid_t pid = (pid_t)target_pid;
	if (::kill(pid, SIGTERM) != 0 && errno != ESRCH) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux::kill_target: kill failed errno=%d", (int)errno));
	}
	int status = 0;
	::waitpid(pid, &status, WNOHANG);
	target_pid = 0;
	if (target_pidfd >= 0) {
		::close(target_pidfd);
		target_pidfd = -1;
	}
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

Error SandboxLinux::lower_token() {
#ifdef LINUXBSD_ENABLED
	ERR_FAIL_COND_V_MSG(!is_target(), ERR_UNAVAILABLE,
			"SandboxLinux: lower_token called from broker (not a sandbox target)");

	const char *force = ::getenv("TG_SANDBOX_FORCE_FAIL");
	if (force != nullptr && force[0] == '1') {
		ERR_FAIL_V_MSG(FAILED,
				"SandboxLinux: lower_token forced to fail by TG_SANDBOX_FORCE_FAIL=1");
	}

	const char *rw_dir = ::getenv("TG_SANDBOX_RW_DIR");
	const String rw = (rw_dir != nullptr) ? String::utf8(rw_dir) : String();

	const char *ro = ::getenv("TG_SANDBOX_RO_FILES");
	Vector<String> ro_files;
	if (ro != nullptr) {
		const PackedStringArray split = String::utf8(ro).split("|", false);
		for (int i = 0; i < split.size(); ++i) {
			ro_files.push_back(split[i]);
		}
	}

	const char *rw_extra = ::getenv("TG_SANDBOX_RW_FILES");
	Vector<String> rw_files;
	if (rw_extra != nullptr) {
		const PackedStringArray split = String::utf8(rw_extra).split("|", false);
		for (int i = 0; i < split.size(); ++i) {
			rw_files.push_back(split[i]);
		}
	}

	const Error err = tg_apply_lockdown(rw, rw_files, ro_files);
	if (err == OK) {
		print_line("SandboxLinux: lockdown engaged (landlock + caps + seccomp)");
	}
	return err;
#else
	return ERR_UNAVAILABLE;
#endif
}

bool SandboxLinux::is_target() const {
#ifdef TG_RENDERER
	return true;
#else
	return false;
#endif
}
