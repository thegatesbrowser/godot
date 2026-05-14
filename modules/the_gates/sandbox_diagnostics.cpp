#include "sandbox_diagnostics.h"

#include "core/io/json.h"
#include "core/string/print_string.h"
#include "core/variant/dictionary.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#include <fileapi.h>
#include <processthreadsapi.h>
#include <sddl.h>
#include <securitybaseapi.h>
#include <winternl.h>
#endif

namespace {

#ifdef WINDOWS_ENABLED

String integrity_label_from_sid_str(const String &sid_str) {
	if (sid_str == "S-1-16-0") {
		return "untrusted";
	}
	if (sid_str == "S-1-16-4096") {
		return "low";
	}
	if (sid_str == "S-1-16-8192") {
		return "medium";
	}
	if (sid_str == "S-1-16-8448") {
		return "medium_plus";
	}
	if (sid_str == "S-1-16-12288") {
		return "high";
	}
	if (sid_str == "S-1-16-16384") {
		return "system";
	}
	return "unknown";
}

Dictionary collect_token_info() {
	Dictionary out;
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		out["token_query"] = "open_failed";
		return out;
	}

	// Integrity level
	DWORD needed = 0;
	GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
	if (needed > 0) {
		Vector<uint8_t> buf;
		buf.resize(needed);
		if (GetTokenInformation(token, TokenIntegrityLevel, buf.ptrw(), needed, &needed)) {
			TOKEN_MANDATORY_LABEL *tml = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buf.ptrw());
			LPSTR sid_str_a = nullptr;
			if (ConvertSidToStringSidA(tml->Label.Sid, &sid_str_a)) {
				String sid_str = String::utf8(sid_str_a);
				out["integrity_sid"] = sid_str;
				out["integrity"] = integrity_label_from_sid_str(sid_str);
				LocalFree(sid_str_a);
			}
		}
	}

	// Restricted SIDs count (USER_LOCKDOWN / USER_RESTRICTED leave only S-1-0-0)
	DWORD restricted_needed = 0;
	GetTokenInformation(token, TokenRestrictedSids, nullptr, 0, &restricted_needed);
	if (restricted_needed > 0) {
		Vector<uint8_t> buf;
		buf.resize(restricted_needed);
		if (GetTokenInformation(token, TokenRestrictedSids, buf.ptrw(), restricted_needed, &restricted_needed)) {
			TOKEN_GROUPS *groups = reinterpret_cast<TOKEN_GROUPS *>(buf.ptrw());
			out["restricted_sid_count"] = (int)groups->GroupCount;
		}
	} else {
		out["restricted_sid_count"] = 0;
	}

	CloseHandle(token);
	return out;
}

Dictionary collect_mitigations() {
	Dictionary out;
	// DEP
	PROCESS_MITIGATION_DEP_POLICY dep = {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessDEPPolicy, &dep, sizeof(dep))) {
		out["dep_enable"] = (bool)dep.Enable;
		out["dep_permanent"] = (bool)dep.Permanent;
	}
	// ASLR
	PROCESS_MITIGATION_ASLR_POLICY aslr = {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessASLRPolicy, &aslr, sizeof(aslr))) {
		out["aslr_force_relocate"] = (bool)aslr.EnableForceRelocateImages;
		out["aslr_bottom_up"] = (bool)aslr.EnableBottomUpRandomization;
		out["aslr_high_entropy"] = (bool)aslr.EnableHighEntropy;
	}
	// Dynamic code (ACG)
	PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn = {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessDynamicCodePolicy, &dyn, sizeof(dyn))) {
		out["dynamic_code_prohibited"] = (bool)dyn.ProhibitDynamicCode;
	}
	// Win32k system calls
	PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY syscall = {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessSystemCallDisablePolicy, &syscall, sizeof(syscall))) {
		out["win32k_disabled"] = (bool)syscall.DisallowWin32kSystemCalls;
	}
	// Strict handle checks
	PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handles = {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessStrictHandleCheckPolicy, &handles, sizeof(handles))) {
		out["strict_handle_checks"] = (bool)handles.RaiseExceptionOnInvalidHandleReference;
	}
	// Heap terminate
	PROCESS_MITIGATION_PAYLOAD_RESTRICTION_POLICY payload = {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessPayloadRestrictionPolicy, &payload, sizeof(payload))) {
		out["payload_restriction"] = true;
	}
	// CFG
	PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg = {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy, &cfg, sizeof(cfg))) {
		out["cfg_enabled"] = (bool)cfg.EnableControlFlowGuard;
	}
	return out;
}

String current_desktop_name() {
	HDESK desk = GetThreadDesktop(GetCurrentThreadId());
	if (!desk) {
		return "";
	}
	wchar_t name[256] = {0};
	DWORD needed = 0;
	GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), &needed);
	return String::utf16((const char16_t *)name);
}

Dictionary run_canaries() {
	Dictionary out;
	// File write canary: try to create a file in user's profile root,
	// somewhere we don't expect the renderer to have access post-lockdown.
	{
		wchar_t profile[MAX_PATH] = {0};
		DWORD plen = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
		if (plen > 0 && plen < MAX_PATH) {
			String path = String::utf16((const char16_t *)profile) + "\\thegates-sandbox-canary.txt";
			HANDLE h = CreateFileW((LPCWSTR)path.utf16().get_data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h != INVALID_HANDLE_VALUE) {
				out["canary_file_write"] = "allowed";
				CloseHandle(h);
				DeleteFileW((LPCWSTR)path.utf16().get_data());
			} else {
				DWORD err = GetLastError();
				out["canary_file_write"] = "blocked";
				out["canary_file_write_error"] = (int)err;
			}
		} else {
			out["canary_file_write"] = "skipped_no_userprofile";
		}
	}
	// Registry write canary: try to open HKCU\Software for write
	{
		HKEY key = nullptr;
		LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_SET_VALUE, &key);
		if (res == ERROR_SUCCESS) {
			out["canary_reg_write"] = "allowed";
			RegCloseKey(key);
		} else {
			out["canary_reg_write"] = "blocked";
			out["canary_reg_write_error"] = (int)res;
		}
	}
	return out;
}

#endif // WINDOWS_ENABLED

} // namespace

void SandboxDiagnostics::dump() {
	Dictionary diag;

#ifdef WINDOWS_ENABLED
	diag["platform"] = "windows";
	diag["pid"] = (int)GetCurrentProcessId();
	diag["build"] = String(
#ifdef THE_GATES_SANDBOX
			"the_gates_sandbox=yes"
#else
			"the_gates_sandbox=no"
#endif
	);

	Dictionary token = collect_token_info();
	for (const Variant *k = token.next(nullptr); k; k = token.next(k)) {
		diag[*k] = token[*k];
	}

	Dictionary mit = collect_mitigations();
	diag["mitigations"] = mit;

	diag["alt_desktop"] = current_desktop_name();

	diag["canaries"] = run_canaries();
#else
	diag["platform"] = "non-windows";
#endif

	String json = JSON::stringify(diag);
	print_line("=== SANDBOX-DIAG-BEGIN ===");
	print_line(json);
	print_line("=== SANDBOX-DIAG-END ===");
}
