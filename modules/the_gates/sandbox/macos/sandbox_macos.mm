/**************************************************************************/
/*  sandbox_macos.mm                                                      */
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

#include "sandbox_macos.h"

#include "../sandbox_policy.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/string/print_string.h"

#ifdef MACOS_ENABLED

#include "Sandbox.h"
#include "seatbelt_profile.h"
#include "signature_verify.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

extern char **environ;
extern "C" int _NSGetExecutablePath(char *, uint32_t *);

// Cross-file hook into the vendored firefox-shim/mac/Sandbox.mm: set
// before mozilla::StartMacSandbox to append extra SBPL after the
// Firefox content profile, cleared after. Read via the matching extern
// declaration at the top of Sandbox.mm.
extern "C" const char *tg_renderer_sandbox_addend = nullptr;

namespace {

void write_broker_policy_json(const String &p_log_path, const String &p_executable, pid_t p_pid,
		const Ref<SandboxPolicy> &p_policy) {
	Dictionary policy = p_policy->to_dict();
	policy["executable"] = p_executable;
	policy["pid"] = (int64_t)p_pid;
	policy["integrity_target"] = "untrusted";
	policy["sandbox_backend"] = "seatbelt";

	const String json_path = p_log_path.get_base_dir().path_join("broker_policy.json");
	Ref<FileAccess> file = FileAccess::open(json_path, FileAccess::WRITE);
	if (file.is_null()) {
		ERR_PRINT(vformat("SandboxMacOS: cannot open %s for write", json_path));
		return;
	}
	file->store_string(JSON::stringify(policy, "\t", false));
}

PackedStringArray split_pipe(const char *p_env_value) {
	PackedStringArray out;
	if (p_env_value == nullptr) {
		return out;
	}
	const PackedStringArray parts = String::utf8(p_env_value).split("|", false);
	for (int i = 0; i < parts.size(); ++i) {
		out.push_back(parts[i]);
	}
	return out;
}

} // namespace

#endif // MACOS_ENABLED

Dictionary SandboxMacOS::spawn_target(const Ref<SandboxPolicy> &p_policy,
		const String &p_executable, const Vector<String> &p_arguments) {
	Dictionary result;
#ifdef MACOS_ENABLED
	ERR_FAIL_COND_V_MSG(p_policy.is_null(), result,
			"SandboxMacOS::spawn_target requires a non-null SandboxPolicy");
	ERR_FAIL_COND_V_MSG(is_target(), result,
			"SandboxMacOS::spawn_target called from a sandbox target process");
	ERR_FAIL_COND_V_MSG(target_pid != 0 && is_target_running(), result,
			vformat("SandboxMacOS::spawn_target: previous target pid=%d still running; call kill_target() first",
					(int)target_pid));

	if (target_kq >= 0) {
		::close(target_kq);
		target_kq = -1;
	}
	target_pid = 0;

	// Build argv.
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

	// Policy crosses execve as TG_SANDBOX_* env vars; matches the Linux protocol.
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
	env_owned.push_back(("TG_SANDBOX_APP_PATH=" + p_executable).utf8());
	env_owned.push_back(String(p_policy->is_audio_allowed() ? "TG_SANDBOX_ALLOW_AUDIO=1" : "TG_SANDBOX_ALLOW_AUDIO=0").utf8());
	env_owned.push_back(String(p_policy->is_microphone_allowed() ? "TG_SANDBOX_ALLOW_MICROPHONE=1" : "TG_SANDBOX_ALLOW_MICROPHONE=0").utf8());

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

	// Set up file_actions to redirect child stdout/stderr to the log path.
	posix_spawn_file_actions_t actions;
	const int actions_init = ::posix_spawn_file_actions_init(&actions);
	if (actions_init != 0) {
		ERR_FAIL_V_MSG(result, vformat(
				"SandboxMacOS::spawn_target: posix_spawn_file_actions_init failed errno=%d", actions_init));
	}

	const CharString log_cs = p_policy->get_child_stdout_log_path().utf8();
	const char *log_path = log_cs.length() > 0 ? log_cs.get_data() : nullptr;
	if (log_path != nullptr) {
		const int rc1 = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
				log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		const int rc2 = ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
		if (rc1 != 0 || rc2 != 0) {
			::posix_spawn_file_actions_destroy(&actions);
			ERR_FAIL_V_MSG(result, vformat(
					"SandboxMacOS::spawn_target: posix_spawn_file_actions setup failed rc1=%d rc2=%d", rc1, rc2));
		}
	}

	pid_t pid = 0;
	const int rc = ::posix_spawn(&pid, exe_cs.get_data(), &actions, nullptr, argv.ptrw(), envp.ptrw());
	::posix_spawn_file_actions_destroy(&actions);
	if (rc != 0) {
		ERR_FAIL_V_MSG(result, vformat("SandboxMacOS::spawn_target: posix_spawn failed errno=%d", rc));
	}

	target_pid = (int64_t)pid;
	target_kq = ::kqueue();
	if (target_kq >= 0) {
		struct kevent kev = {};
		EV_SET(&kev, pid, EVFILT_PROC, EV_ADD | EV_CLEAR, NOTE_EXIT, 0, nullptr);
		if (::kevent(target_kq, &kev, 1, nullptr, 0, nullptr) != 0) {
			::close(target_kq);
			target_kq = -1;
		}
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

SandboxMacOS::~SandboxMacOS() {
#ifdef MACOS_ENABLED
	if (target_kq >= 0) {
		::close(target_kq);
		target_kq = -1;
	}
#endif
}

void SandboxMacOS::apply_renderer_acl(const String &p_path) {
	// Same-uid + Seatbelt profile gates real access; no pre-spawn DACL stamp.
	(void)p_path;
}

Error SandboxMacOS::_verify_binary_impl(const String &p_path) {
#ifdef MACOS_ENABLED
	const char *force = ::getenv("TG_SIGNATURE_FORCE_FAIL");
	if (force != nullptr && force[0] == '1') {
		ERR_FAIL_V_MSG(ERR_UNAUTHORIZED,
				"SandboxMacOS::verify_binary forced to fail by TG_SIGNATURE_FORCE_FAIL=1");
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

bool SandboxMacOS::is_target_running() const {
#ifdef MACOS_ENABLED
	if (target_pid == 0) {
		return false;
	}
	if (target_kq >= 0) {
		struct timespec ts = { 0, 0 };
		struct kevent out = {};
		const int n = ::kevent(target_kq, nullptr, 0, &out, 1, &ts);
		if (n > 0 && (out.fflags & NOTE_EXIT) != 0) {
			return false;
		}
		if (n == 0) {
			return true;
		}
	}
	return ::kill((pid_t)target_pid, 0) == 0;
#else
	return false;
#endif
}

Error SandboxMacOS::kill_target() {
#ifdef MACOS_ENABLED
	if (target_pid == 0) {
		return ERR_DOES_NOT_EXIST;
	}
	const pid_t pid = (pid_t)target_pid;
	if (::kill(pid, SIGTERM) != 0 && errno != ESRCH) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxMacOS::kill_target: kill failed errno=%d", (int)errno));
	}
	int status = 0;
	::waitpid(pid, &status, WNOHANG);
	target_pid = 0;
	if (target_kq >= 0) {
		::close(target_kq);
		target_kq = -1;
	}
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

Error SandboxMacOS::lower_token() {
#ifdef MACOS_ENABLED
	ERR_FAIL_COND_V_MSG(!is_target(), ERR_UNAVAILABLE,
			"SandboxMacOS: lower_token called from broker (not a sandbox target)");

	const char *force = ::getenv("TG_SANDBOX_FORCE_FAIL");
	if (force != nullptr && force[0] == '1') {
		ERR_FAIL_V_MSG(FAILED,
				"SandboxMacOS: lower_token forced to fail by TG_SANDBOX_FORCE_FAIL=1");
	}

	const char *rw_dir_env = ::getenv("TG_SANDBOX_RW_DIR");
	const String rw_dir = (rw_dir_env != nullptr) ? String::utf8(rw_dir_env) : String();

	const char *app_path_env = ::getenv("TG_SANDBOX_APP_PATH");
	String app_path = (app_path_env != nullptr) ? String::utf8(app_path_env) : String();
	if (app_path.is_empty()) {
		char buf[1024] = { 0 };
		uint32_t bufsize = sizeof(buf);
		if (_NSGetExecutablePath(buf, &bufsize) == 0) {
			app_path = String::utf8(buf);
		}
	}

	const PackedStringArray ro_files = split_pipe(::getenv("TG_SANDBOX_RO_FILES"));
	const char *allow_audio_env = ::getenv("TG_SANDBOX_ALLOW_AUDIO");
	const bool allow_audio = allow_audio_env != nullptr && allow_audio_env[0] == '1';
	const char *allow_mic_env = ::getenv("TG_SANDBOX_ALLOW_MICROPHONE");
	const bool allow_microphone = allow_mic_env != nullptr && allow_mic_env[0] == '1';

	MacSandboxInfo info;
	info.type = MacSandboxType_Content;
	info.level = 3;
	info.hasFilePrivileges = false;
	info.hasSandboxedProfile = !rw_dir.is_empty();
	// hasAudio=true would pull in Firefox's audio addend which unconditionally
	// includes (allow device-microphone). We build audio/mic rules ourselves.
	info.hasAudio = false;
	info.hasWindowServer = true;
	info.shouldLog = ::getenv("TG_SANDBOX_LOG_SBPL") != nullptr;
	info.appPath = std::string(app_path.utf8().get_data());
	info.profileDir = std::string(rw_dir.utf8().get_data());
	if (ro_files.size() > 0) {
		info.testingReadPath1 = std::string(ro_files[0].utf8().get_data());
	}
	if (ro_files.size() > 1) {
		info.testingReadPath2 = std::string(ro_files[1].utf8().get_data());
	}
	if (ro_files.size() > 2) {
		info.testingReadPath3 = std::string(ro_files[2].utf8().get_data());
	}
	if (ro_files.size() > 3) {
		info.testingReadPath4 = std::string(ro_files[3].utf8().get_data());
	}

	// addend_utf8 must outlive StartMacSandbox: the extern hook holds a
	// raw const char* that the call reads synchronously.
	const String addend = tg_build_seatbelt_addend(allow_audio, allow_microphone);
	const CharString addend_utf8 = addend.utf8();
	tg_renderer_sandbox_addend = addend_utf8.get_data();
	std::string err_msg;
	const bool ok = mozilla::StartMacSandbox(info, err_msg);
	tg_renderer_sandbox_addend = nullptr;

	if (!ok) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxMacOS::lower_token: StartMacSandbox failed: %s",
				String::utf8(err_msg.c_str())));
	}
	print_line("SandboxMacOS: lockdown engaged (Seatbelt profile applied)");
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

bool SandboxMacOS::is_target() const {
#ifdef TG_RENDERER
	return true;
#else
	return false;
#endif
}
