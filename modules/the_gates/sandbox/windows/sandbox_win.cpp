/**************************************************************************/
/*  sandbox_win.cpp                                                       */
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

#include "sandbox_win.h"

#include "../sandbox_policy.h"
#include "../socket_acl.h"
#include "broker_delegate.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/string/print_string.h"
#include "handle_scope.h"
#include "signature_verify.h"
#include "string_utils.h"

#include <memory>
#include <string>

#include <sandbox/win/src/sandbox.h>
#include <sandbox/win/src/sandbox_factory.h>
#include <sandbox/win/src/sandbox_policy.h>
#include <sandbox/win/src/security_level.h>

#include <windows.h>

namespace {

// HKLM key the elevated tg-wfp-tool writes during install (and clears during
// uninstall). The user-mode launcher reads it because WFP filter / provider
// enumeration is denied to non-admin callers — the BFE's default DACL hides
// the filter list from BUILTIN\Users. Admin to write, anyone to read.
constexpr const wchar_t *kWFPInstallMarkerKey = L"SOFTWARE\\TheGates\\NetworkFilter";

bool read_wfp_install_marker(uint32_t *r_filter_count) {
	HKEY key = nullptr;
	LSTATUS res = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kWFPInstallMarkerKey,
			0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
	if (res != ERROR_SUCCESS) {
		return false;
	}
	DWORD installed = 0;
	DWORD value_size = sizeof(installed);
	res = RegQueryValueExW(key, L"Installed", nullptr, nullptr,
			reinterpret_cast<BYTE *>(&installed), &value_size);
	if (res != ERROR_SUCCESS || installed != 1) {
		RegCloseKey(key);
		return false;
	}
	if (r_filter_count != nullptr) {
		DWORD filter_count = 0;
		value_size = sizeof(filter_count);
		if (RegQueryValueExW(key, L"FilterCount", nullptr, nullptr,
					reinterpret_cast<BYTE *>(&filter_count), &value_size) == ERROR_SUCCESS) {
			*r_filter_count = filter_count;
		}
	}
	RegCloseKey(key);
	return true;
}

bool force_network_filter_fail() {
	wchar_t buf[8] = { 0 };
	return ::GetEnvironmentVariableW(L"TG_NETWORK_FILTER_FORCE_FAIL", buf, 8) > 0 && buf[0] == L'1';
}

void write_broker_policy_json(const String &p_log_path, const String &p_executable, DWORD p_pid,
		const Ref<SandboxPolicy> &p_policy) {
	Dictionary policy = p_policy->to_dict();
	policy["executable"] = p_executable;
	policy["pid"] = (int64_t)p_pid;
	policy["token_initial"] = "USER_RESTRICTED_SAME_ACCESS";
	policy["token_lockdown"] = "USER_LIMITED";
	policy["integrity_target"] = "UNTRUSTED";
	policy["job_level"] = "LOCKDOWN";
	policy["alternate_desktop"] = true;

	PackedStringArray mit_initial;
	mit_initial.push_back("DEP");
	mit_initial.push_back("DEP_NO_ATL_THUNK");
	mit_initial.push_back("SEHOP");
	mit_initial.push_back("BOTTOM_UP_ASLR");
	mit_initial.push_back("HIGH_ENTROPY_ASLR");
	policy["mitigations_initial"] = mit_initial;

	PackedStringArray mit_delayed;
	mit_delayed.push_back("DLL_SEARCH_ORDER");
	policy["mitigations_delayed"] = mit_delayed;

	const String json_path = p_log_path.get_base_dir().path_join("broker_policy.json");
	Ref<FileAccess> file = FileAccess::open(json_path, FileAccess::WRITE);
	if (file.is_null()) {
		ERR_PRINT(vformat("SandboxWin: cannot open %s for write", json_path));
		return;
	}
	file->store_string(JSON::stringify(policy, "\t", false));
}

std::wstring build_command_line(const String &p_executable, const Vector<String> &p_arguments) {
	std::wstring out;
	out.reserve(256);
	out.push_back(L'"');
	out += tg_to_wide(p_executable);
	out.push_back(L'"');
	for (int i = 0; i < p_arguments.size(); ++i) {
		out.push_back(L' ');
		const std::wstring arg = tg_to_wide(p_arguments[i]);
		const bool needs_quotes = arg.find_first_of(L" \t") != std::wstring::npos;
		if (needs_quotes) {
			out.push_back(L'"');
		}
		out += arg;
		if (needs_quotes) {
			out.push_back(L'"');
		}
	}
	return out;
}

} // namespace

Dictionary SandboxWin::spawn_target(const Ref<SandboxPolicy> &p_policy,
		const String &p_executable, const Vector<String> &p_arguments) {
	Dictionary result;
	ERR_FAIL_COND_V_MSG(p_policy.is_null(), result,
			"SandboxWin::spawn_target requires a non-null SandboxPolicy");
	if (broker_service == nullptr) {
		ERR_PRINT("SandboxWin::spawn_target called from a sandbox target process");
		return result;
	}

	// Fail-closed: refuse to spawn if the WFP filter the installer was supposed
	// to register isn't present. Marker is written by tg-wfp-tool.exe during
	// the installer's elevated phase; absence means either the install step
	// was skipped or someone cleared it. Either way, don't run with the
	// renderer's outbound private-network access unrestricted.
	if (p_policy->is_private_networks_blocked()) {
		if (force_network_filter_fail()) {
			ERR_PRINT("SandboxWin: TG_NETWORK_FILTER_FORCE_FAIL=1 — refusing to spawn renderer.");
			return result;
		}
		uint32_t filter_count = 0;
		if (!read_wfp_install_marker(&filter_count)) {
			ERR_PRINT("SandboxWin: WFP install marker missing under "
					  "HKLM\\SOFTWARE\\TheGates\\NetworkFilter. "
					  "Run the installer's WFP setup step (tg-wfp-tool.exe install).");
			return result;
		}
		print_line(vformat("SandboxWin: %d WFP filters registered (per install marker)", (int)filter_count));
	}

	const String stdout_log_path = p_policy->get_child_stdout_log_path();
	const String rw_dir = p_policy->get_rw_dir();
	const PackedStringArray rw_files = p_policy->get_rw_files();
	const PackedStringArray ro_files = p_policy->get_ro_files();

	if (!broker_initialized) {
		auto delegate = std::make_unique<BrokerDelegate>();
		sandbox::ResultCode init = broker_service->Init(std::move(delegate));
		if (init != sandbox::SBOX_ALL_OK) {
			ERR_PRINT(vformat("SandboxWin: BrokerServices::Init failed (%d)", (int)init));
			return result;
		}
		broker_initialized = true;
	}

	std::unique_ptr<sandbox::TargetPolicy> policy = broker_service->CreatePolicy();
	sandbox::TargetConfig *config = policy->GetConfig();

	sandbox::ResultCode r;

	// Job restrictions: lockdown blocks children, clipboard, global hooks.
	r = config->SetJobLevel(sandbox::JobLevel::kLockdown, /*ui_exceptions=*/0);
	ERR_FAIL_COND_V_MSG(r != sandbox::SBOX_ALL_OK, result, vformat("SandboxWin: SetJobLevel failed (%d)", (int)r));

	// Initial / lockdown token levels. USER_LOCKDOWN breaks IPC (renderer
	// can't open the launcher's AF_UNIX sockets); USER_LIMITED keeps IPC.
	r = config->SetTokenLevel(sandbox::TokenLevel::USER_RESTRICTED_SAME_ACCESS,
			sandbox::TokenLevel::USER_LIMITED);
	ERR_FAIL_COND_V_MSG(r != sandbox::SBOX_ALL_OK, result, vformat("SandboxWin: SetTokenLevel failed (%d)", (int)r));

	r = broker_service->CreateAlternateDesktop(sandbox::Desktop::kAlternateDesktop);
	if (r != sandbox::SBOX_ALL_OK && r != sandbox::SBOX_ERROR_GENERIC) {
		ERR_PRINT(vformat("SandboxWin: CreateAlternateDesktop returned %d (continuing)", (int)r));
	}
	config->SetDesktop(sandbox::Desktop::kAlternateDesktop);

	config->SetDelayedIntegrityLevel(sandbox::IntegrityLevel::INTEGRITY_LEVEL_UNTRUSTED);

	// Chromium's policy matcher uses NT-style paths (backslashes); single-asterisk globs
	// don't cross path separators, so a dir needs one rule per depth level.
	auto path_to_wstring = [](const String &p_path) {
		const String normalized = p_path.replace_char('/', '\\');
		const Char16String utf16 = normalized.utf16();
		return std::wstring((const wchar_t *)utf16.get_data());
	};
	auto allow_file = [&](sandbox::FileSemantics sem, const std::wstring &pattern) {
		sandbox::ResultCode rf = config->AllowFileAccess(sem, pattern.c_str());
		if (rf != sandbox::SBOX_ALL_OK) {
			ERR_PRINT(vformat("SandboxWin: AllowFileAccess(%s) returned %d",
					String::utf16((const char16_t *)pattern.c_str()), (int)rf));
		}
	};
	auto allow_dir_recursive = [&](sandbox::FileSemantics sem, const String &p_dir) {
		// 3 levels caps the per-dir rules at ~15 (5 services × 3 depths); higher
		// depths overflow chromium-sandbox's 24 KB policy buffer with FAILED_TO_FREEZE_CONFIG.
		const std::wstring root = path_to_wstring(p_dir);
		static const wchar_t *globs[] = {
			L"\\*",
			L"\\*\\*",
			L"\\*\\*\\*",
		};
		for (const wchar_t *suffix : globs) {
			allow_file(sem, root + suffix);
		}
	};

	ERR_FAIL_COND_V_MSG(rw_dir.is_empty(), result,
			"SandboxWin: spawn_target requires policy.rw_dir (per-gate user data dir)");

	allow_dir_recursive(sandbox::FileSemantics::kAllowAny, rw_dir);
	for (int i = 0; i < rw_files.size(); ++i) {
		allow_file(sandbox::FileSemantics::kAllowAny, path_to_wstring(rw_files[i]));
	}
	for (int i = 0; i < ro_files.size(); ++i) {
		const String &ro_path = ro_files[i];
		const DWORD attrs = ::GetFileAttributesW((const wchar_t *)ro_path.utf16().get_data());
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			allow_dir_recursive(sandbox::FileSemantics::kAllowReadonly, ro_path);
		} else {
			allow_file(sandbox::FileSemantics::kAllowReadonly, path_to_wstring(ro_path));
		}
	}

	r = config->SetProcessMitigations(
			sandbox::MITIGATION_DEP |
			sandbox::MITIGATION_DEP_NO_ATL_THUNK |
			sandbox::MITIGATION_SEHOP |
			sandbox::MITIGATION_BOTTOM_UP_ASLR |
			sandbox::MITIGATION_HIGH_ENTROPY_ASLR |
			sandbox::MITIGATION_RESTRICT_INDIRECT_BRANCH_PREDICTION);
	if (r != sandbox::SBOX_ALL_OK) {
		ERR_PRINT(vformat("SandboxWin: SetProcessMitigations returned %d", (int)r));
	}

	r = config->SetDelayedProcessMitigations(
			sandbox::MITIGATION_DLL_SEARCH_ORDER);
	if (r != sandbox::SBOX_ALL_OK) {
		ERR_PRINT(vformat("SandboxWin: SetDelayedProcessMitigations returned %d", (int)r));
	}

	const std::wstring exe_wide = tg_to_wide(p_executable);
	const std::wstring cmd_wide = build_command_line(p_executable, p_arguments);

	// Pre-open the log at broker integrity — the locked-down child inherits a writable handle.
	HandleScope log_handle;
	if (!stdout_log_path.is_empty()) {
		const std::wstring log_path_wide = tg_to_wide(stdout_log_path);
		SECURITY_ATTRIBUTES sa = {};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		log_handle.reset(::CreateFileW(
				log_path_wide.c_str(),
				FILE_GENERIC_WRITE | FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				&sa,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr));
		if (!log_handle.is_valid()) {
			ERR_PRINT(vformat("SandboxWin: CreateFile(log) failed win=%d", (int)::GetLastError()));
		} else {
			sandbox::ResultCode rs = policy->SetStdoutHandle(log_handle.get());
			if (rs != sandbox::SBOX_ALL_OK) {
				ERR_PRINT(vformat("SandboxWin: SetStdoutHandle returned %d", (int)rs));
			}
			rs = policy->SetStderrHandle(log_handle.get());
			if (rs != sandbox::SBOX_ALL_OK) {
				ERR_PRINT(vformat("SandboxWin: SetStderrHandle returned %d", (int)rs));
			}
		}
	}

	DWORD last_error = 0;
	PROCESS_INFORMATION pi = {};
	sandbox::ResultCode spawn = broker_service->SpawnTarget(
			exe_wide.c_str(), cmd_wide.c_str(), std::move(policy), &last_error, &pi);
	if (spawn != sandbox::SBOX_ALL_OK) {
		ERR_PRINT(vformat("SandboxWin: SpawnTarget failed (sbox=%d, win=%d)",
				(int)spawn, (int)last_error));
		return result;
	}

	// SpawnTarget creates the child suspended.
	if (pi.hThread) {
		::ResumeThread(pi.hThread);
	}

	target_process.reset(pi.hProcess);
	target_thread.reset(pi.hThread);
	target_pid = (int64_t)pi.dwProcessId;

	result["pid"] = (int64_t)pi.dwProcessId;
	result["thread_id"] = (int64_t)pi.dwThreadId;

	if (!stdout_log_path.is_empty()) {
		write_broker_policy_json(stdout_log_path, p_executable, pi.dwProcessId, p_policy);
	}

	return result;
}

void SandboxWin::apply_renderer_acl(const String &p_path) {
	tg_apply_untrusted_acl(p_path);
}

Error SandboxWin::_verify_binary_impl(const String &p_path) {
	// Negative-test hook: harness forces signature verify to fail to confirm
	// the launcher refuses to spawn. Never set in production.
	wchar_t force[8] = { 0 };
	if (::GetEnvironmentVariableW(L"TG_SIGNATURE_FORCE_FAIL", force, 8) > 0 && force[0] == L'1') {
		ERR_FAIL_V_MSG(ERR_UNAUTHORIZED,
				"SandboxWin::verify_binary forced to fail by TG_SIGNATURE_FORCE_FAIL=1");
	}

#ifdef TG_SIGNATURE_PIN
	return tg_verify_renderer_binary(p_path, String(TG_SIGNATURE_PIN));
#else
	print_line(vformat("[VERIFY-BYPASSED] signature_check disabled at build time for %s (no tg_signature_pin)", p_path));
	return OK;
#endif
}

bool SandboxWin::is_target_running() const {
	if (!target_process.is_valid()) {
		return false;
	}
	// GetExitCodeProcess + STILL_ACTIVE comparison is unreliable: a process
	// that exits with code 259 looks alive. WaitForSingleObject is the
	// recommended probe (Microsoft Learn: GetExitCodeProcess remarks).
	return ::WaitForSingleObject(target_process.get(), 0) == WAIT_TIMEOUT;
}

Error SandboxWin::kill_target() {
	if (!target_process.is_valid()) {
		return ERR_DOES_NOT_EXIST;
	}
	if (!::TerminateProcess(target_process.get(), 1)) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxWin::kill_target: TerminateProcess failed (win=%d)", (int)::GetLastError()));
	}
	return OK;
}

Error SandboxWin::lower_token() {
	ERR_FAIL_COND_V_MSG(!is_target(), ERR_UNAVAILABLE,
			"SandboxWin: lower_token called from broker (not a sandbox target)");

	// Negative-test hook: the harness can force a lockdown failure to verify
	// the caller aborts (fail-closed). Never set in production launchers.
	wchar_t force[8] = { 0 };
	if (::GetEnvironmentVariableW(L"TG_SANDBOX_FORCE_FAIL", force, 8) > 0 && force[0] == L'1') {
		ERR_FAIL_V_MSG(FAILED, "SandboxWin: lower_token forced to fail by TG_SANDBOX_FORCE_FAIL=1");
	}

	sandbox::TargetServices *target_service = sandbox::SandboxFactory::GetTargetServices();
	ERR_FAIL_COND_V_MSG(target_service == nullptr, FAILED,
			"SandboxWin: GetTargetServices returned null");

	sandbox::ResultCode init = target_service->Init();
	ERR_FAIL_COND_V_MSG(init != sandbox::SBOX_ALL_OK, FAILED,
			vformat("SandboxWin: TargetServices::Init failed (%d)", (int)init));

	target_service->LowerToken();
	print_line("SandboxWin: LowerToken complete; renderer is now sandboxed");
	return OK;
}

Dictionary SandboxWin::network_state() const {
	Dictionary out;
	uint32_t filter_count = 0;
	const bool installed = read_wfp_install_marker(&filter_count);
	out["installed"] = installed;
	out["filter_count"] = (int)filter_count;
	out["status"] = installed ? "active" : "no_filters_installed";
	return out;
}

bool SandboxWin::broker_initialized = false;

SandboxWin::SandboxWin() {
	broker_service = sandbox::SandboxFactory::GetBrokerServices();
}
