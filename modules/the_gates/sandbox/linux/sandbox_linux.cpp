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
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

// Inherited control-channel FD slot the renderer reads via --tg-broker-fd.
// Fixed at 3 so it lands in a deterministic slot across the spawn boundary;
// the child dup2s the socketpair half there before launching the renderer.
constexpr int CHILD_BROKER_FD_NUM = 3;

// Env vars passed through to the renderer. Anything not in this allowlist is
// dropped — keeps SSH_AUTH_SOCK, DBUS_SESSION_BUS_ADDRESS, AWS_*, OPENAI_API_KEY
// and any other launcher-process secret out of the gate's environ.
constexpr const char *kEnvAllowExact[] = {
	"HOME",
	"USER",
	"LOGNAME",
	"LANG",
	"LC_ALL",
	"LC_COLLATE",
	"LC_CTYPE",
	"LC_MESSAGES",
	"LC_MONETARY",
	"LC_NUMERIC",
	"LC_TIME",
	"TZ",
	"TERM",
	"PATH",
	"XDG_RUNTIME_DIR",
	"XDG_DATA_HOME",
	"XDG_CONFIG_HOME",
	"XDG_CACHE_HOME",
	"XDG_SESSION_TYPE",
	"XDG_CURRENT_DESKTOP",
	"DISPLAY",
	"WAYLAND_DISPLAY",
	"GDK_BACKEND",
	"QT_QPA_PLATFORM",
	"SDL_VIDEODRIVER",
	"PULSE_SERVER",
	"PIPEWIRE_RUNTIME_DIR",
};

// Prefix matches for vendor-driver env knobs. Renderer / Vulkan / Mesa /
// DXVK / AMD / NVIDIA all use prefixed env vars to tune behavior; passing
// them through keeps driver debug + tuning workflows alive.
constexpr const char *kEnvAllowPrefixes[] = {
	"TG_",
	"VK_",
	"MESA_",
	"LIBGL_",
	"__GL_",
	"NV_",
	"RADV_",
	"AMD_",
	"DXVK_",
};

bool env_var_allowed(const char *p_env) {
	const char *eq = ::strchr(p_env, '=');
	const size_t name_len = (eq != nullptr) ? (size_t)(eq - p_env) : ::strlen(p_env);
	for (const char *name : kEnvAllowExact) {
		if (::strlen(name) == name_len && ::memcmp(p_env, name, name_len) == 0) {
			return true;
		}
	}
	for (const char *prefix : kEnvAllowPrefixes) {
		const size_t plen = ::strlen(prefix);
		if (name_len >= plen && ::memcmp(p_env, prefix, plen) == 0) {
			return true;
		}
	}
	return false;
}

// Parses the pipe-separated string the launcher uses to ship rw_files /
// ro_files through environment variables back into a Vector<String>.
Vector<String> parse_pipe_list(const char *p_env_value) {
	Vector<String> out;
	if (p_env_value == nullptr || p_env_value[0] == '\0') {
		return out;
	}
	const PackedStringArray split = String::utf8(p_env_value).split("|", false);
	for (int i = 0; i < split.size(); ++i) {
		out.push_back(split[i]);
	}
	return out;
}

// Builds the renderer's argv: [exe, p_arguments..., --tg-broker-fd=<n>].
// The CharString storage backs r_argv; both must outlive the exec call,
// which means both live in the caller's stack across fork().
void build_child_argv(const String &p_executable,
		const Vector<String> &p_arguments,
		Vector<CharString> &r_storage,
		Vector<char *> &r_argv) {
	r_storage.push_back(p_executable.utf8());
	for (int i = 0; i < p_arguments.size(); ++i) {
		r_storage.push_back(p_arguments[i].utf8());
	}
	r_storage.push_back(vformat("--tg-broker-fd=%d", CHILD_BROKER_FD_NUM).utf8());
	for (int i = 0; i < r_storage.size(); ++i) {
		r_argv.push_back(const_cast<char *>(r_storage[i].get_data()));
	}
	r_argv.push_back(nullptr);
}

// Builds envp for the renderer: launcher's filtered environ + the
// TG_SANDBOX_* policy vars lower_token() reads back. Same lifetime rule.
void build_child_envp(const Ref<SandboxPolicy> &p_policy,
		Vector<CharString> &r_storage,
		Vector<char *> &r_envp) {
	if (!p_policy->get_rw_dir().is_empty()) {
		r_storage.push_back(("TG_SANDBOX_RW_DIR=" + p_policy->get_rw_dir()).utf8());
	}
	const PackedStringArray rw_files = p_policy->get_rw_files();
	if (rw_files.size() > 0) {
		r_storage.push_back(("TG_SANDBOX_RW_FILES=" + String("|").join(rw_files)).utf8());
	}
	const PackedStringArray ro_files = p_policy->get_ro_files();
	if (ro_files.size() > 0) {
		r_storage.push_back(("TG_SANDBOX_RO_FILES=" + String("|").join(ro_files)).utf8());
	}
	r_storage.push_back(String(p_policy->is_audio_allowed()
					? "TG_SANDBOX_ALLOW_AUDIO=1"
					: "TG_SANDBOX_ALLOW_AUDIO=0")
					.utf8());
	r_storage.push_back(String(p_policy->is_microphone_allowed()
					? "TG_SANDBOX_ALLOW_MICROPHONE=1"
					: "TG_SANDBOX_ALLOW_MICROPHONE=0")
					.utf8());

	for (char **e = environ; *e != nullptr; ++e) {
		if (env_var_allowed(*e)) {
			r_envp.push_back(*e);
		}
	}
	for (int i = 0; i < r_storage.size(); ++i) {
		r_envp.push_back(const_cast<char *>(r_storage[i].get_data()));
	}
	r_envp.push_back(nullptr);
}

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

void child_exec_after_log_dup(const char *log_path, int broker_fd_src, int broker_fd_dst,
		char *const argv[], char *const envp[]) {
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
	if (broker_fd_src >= 0) {
		if (::dup2(broker_fd_src, broker_fd_dst) < 0) {
			static const char msg[] = "tg_spawn: dup2(broker_fd) failed\n";
			const ssize_t w = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
			(void)w;
			::_exit(127);
		}
		if (broker_fd_src != broker_fd_dst) {
			::close(broker_fd_src);
		}
		// Clear FD_CLOEXEC; the broker FD must survive execve.
		const int flags = ::fcntl(broker_fd_dst, F_GETFD, 0);
		if (flags >= 0) {
			::fcntl(broker_fd_dst, F_SETFD, flags & ~FD_CLOEXEC);
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

	ERR_FAIL_COND_V_MSG(target_pid != 0 && is_target_running(), result,
			vformat("SandboxLinux::spawn_target: previous target pid=%d still running; call kill_target() first",
					(int)target_pid));
	if (target_pidfd >= 0) {
		::close(target_pidfd);
		target_pidfd = -1;
	}
	target_pid = 0;

	Vector<CharString> arg_storage;
	Vector<char *> argv;
	build_child_argv(p_executable, p_arguments, arg_storage, argv);

	Vector<CharString> env_storage;
	Vector<char *> envp;
	build_child_envp(p_policy, env_storage, envp);

	const CharString log_cs = p_policy->get_child_stdout_log_path().utf8();
	const char *log_path = log_cs.length() > 0 ? log_cs.get_data() : nullptr;

	// Network isolation: pre-spawn socketpair gives the renderer a kernel
	// FD the broker thread can write to without filesystem rendezvous. The
	// renderer-side seccomp filter denies socket()/connect()/bind(), so
	// the inherited FD is the only path to a network socket. See
	// modules/the_gates/network/ and notes/Sandboxing/Network Isolation.md.
	int broker_sv[2] = { -1, -1 };
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, broker_sv) != 0) {
		ERR_FAIL_V_MSG(result, vformat("SandboxLinux::spawn_target: socketpair failed errno=%d", (int)errno));
	}
	const int launcher_broker_fd = broker_sv[0];
	const int child_broker_fd = broker_sv[1];

	const pid_t pid = ::fork();
	if (pid < 0) {
		::close(launcher_broker_fd);
		::close(child_broker_fd);
		ERR_FAIL_V_MSG(result, vformat("SandboxLinux::spawn_target: fork failed errno=%d", (int)errno));
	}
	if (pid == 0) {
		::close(launcher_broker_fd);
		child_exec_after_log_dup(log_path, child_broker_fd, CHILD_BROKER_FD_NUM,
				argv.ptrw(), envp.ptrw());
	}
	::close(child_broker_fd);

	target_pid = (int64_t)pid;
	start_broker(launcher_broker_fd);
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
		stop_broker();
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
	stop_broker();
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

	const char *rw_dir_env = ::getenv("TG_SANDBOX_RW_DIR");
	const String rw_dir = (rw_dir_env != nullptr) ? String::utf8(rw_dir_env) : String();
	const Vector<String> rw_files = parse_pipe_list(::getenv("TG_SANDBOX_RW_FILES"));
	const Vector<String> ro_files = parse_pipe_list(::getenv("TG_SANDBOX_RO_FILES"));

	// Default deny on missing env vars: a renderer spawned without the
	// audio flags should not silently re-open the audio sockets.
	const char *audio_env = ::getenv("TG_SANDBOX_ALLOW_AUDIO");
	const bool allow_audio = audio_env != nullptr && audio_env[0] == '1';
	const char *mic_env = ::getenv("TG_SANDBOX_ALLOW_MICROPHONE");
	const bool allow_microphone = mic_env != nullptr && mic_env[0] == '1';

	const Error err = tg_apply_lockdown(rw_dir, rw_files, ro_files, allow_audio, allow_microphone);
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
