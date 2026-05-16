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
#include "core/string/print_string.h"

#ifdef MACOS_ENABLED

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

// sandbox_init / sandbox_init_with_parameters are private SPI. The headers
// are in <sandbox.h> on the system but the calls are not on the public ABI.
// Stable since 10.5; Firefox content processes and Chrome renderers ride
// the same SPI. Forward-declared here to avoid pulling the system header
// into a Godot TU that doesn't need it.
extern "C" {
int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
void sandbox_free_error(char *errorbuf);
}

extern char **environ;

#endif // MACOS_ENABLED

Dictionary SandboxMacOS::spawn_target(const Ref<SandboxPolicy> &p_policy,
		const String &p_executable, const Vector<String> &p_arguments) {
	Dictionary result;
#ifdef MACOS_ENABLED
	ERR_FAIL_COND_V_MSG(p_policy.is_null(), result,
			"SandboxMacOS::spawn_target requires a non-null SandboxPolicy");

	CharString exe_cs = p_executable.utf8();
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

	setenv("TG_TARGET", "1", 1);

	pid_t pid = 0;
	if (posix_spawn(&pid, exe_cs.get_data(), nullptr, nullptr, argv.ptrw(), environ) != 0) {
		unsetenv("TG_TARGET");
		ERR_FAIL_V_MSG(result, vformat("SandboxMacOS::spawn_target: posix_spawn failed errno=%d", errno));
	}
	unsetenv("TG_TARGET");

	target_pid = (int64_t)pid;
	result["pid"] = target_pid;
#else
	(void)p_policy;
	(void)p_executable;
	(void)p_arguments;
#endif
	return result;
}

void SandboxMacOS::apply_renderer_acl(const String &p_path) {
	// POSIX: AF_UNIX socket file permissions inherit from process umask +
	// parent dir. Seatbelt profile gates real access — no ACL stamping.
	(void)p_path;
}

Error SandboxMacOS::verify_binary(const String &p_path) {
	// TODO(Phase 3 platform-side): SecStaticCodeCheckValidity against an
	// embedded cert thumbprint (tg_signature_pin SCons flag).
	(void)p_path;
	print_line("[VERIFY-BYPASSED] SandboxMacOS: codesign verify not yet implemented");
	return OK;
}

bool SandboxMacOS::is_target_running() const {
#ifdef MACOS_ENABLED
	if (target_pid == 0) {
		return false;
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
	if (::kill((pid_t)target_pid, SIGTERM) != 0) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxMacOS::kill_target: kill failed errno=%d", errno));
	}
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

Error SandboxMacOS::lower_token() {
#ifdef MACOS_ENABLED
	// Minimal Seatbelt profile: deny everything except the per-gate dir,
	// the .pck, and the IPC sockets the SandboxPolicy declared. The full
	// profile is constructed from SandboxPolicy in Phase 3 follow-up;
	// today we use a conservative built-in template.
	// TODO(Phase 3 platform-side): generate the profile from p_policy
	// fields and parameterise via sandbox_init_with_parameters.
	static const char *profile =
			"(version 1)\n"
			"(deny default)\n"
			"(allow file-read*)\n"
			"(allow file-write* (regex \"^/private/var/folders/.*/T/\"))\n"
			"(allow mach-lookup)\n"
			"(allow ipc-posix-shm)\n"
			"(allow process-fork)\n"
			"(allow signal (target self))\n"
			"(allow sysctl-read)\n"
			"(allow iokit-open)\n";

	char *errbuf = nullptr;
	if (sandbox_init(profile, 0, &errbuf) != 0) {
		String msg = "SandboxMacOS::lower_token: sandbox_init failed";
		if (errbuf) {
			msg += String(" — ") + String::utf8(errbuf);
			sandbox_free_error(errbuf);
		}
		ERR_FAIL_V_MSG(FAILED, msg);
	}
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

bool SandboxMacOS::is_target() const {
#ifdef MACOS_ENABLED
	const char *flag = ::getenv("TG_TARGET");
	return flag != nullptr && flag[0] == '1';
#else
	return false;
#endif
}
