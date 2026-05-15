/**************************************************************************/
/*  sandbox_win.cpp                                                       */
/**************************************************************************/

#include "sandbox_win.h"

#include "BrokerServicesDelegateImpl.h"

#include "core/string/print_string.h"
#include "platform/windows/os_windows.h"

#include <memory>
#include <string>

#include <sandbox/win/src/sandbox.h>
#include <sandbox/win/src/sandbox_factory.h>
#include <sandbox/win/src/sandbox_policy.h>
#include <sandbox/win/src/security_level.h>

#include <windows.h>

namespace {

std::wstring to_wide(const String &p_string) {
	CharString utf8 = p_string.utf8();
	int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.get_data(), -1, nullptr, 0);
	if (wlen <= 0) {
		return {};
	}
	std::wstring out;
	out.resize(static_cast<size_t>(wlen - 1));
	if (wlen > 1) {
		::MultiByteToWideChar(CP_UTF8, 0, utf8.get_data(), -1, out.data(), wlen);
	}
	return out;
}

// Build a Windows-style command line, quoting arguments that contain spaces.
std::wstring build_command_line(const String &p_executable, const Vector<String> &p_arguments) {
	std::wstring out;
	out.reserve(256);
	out.push_back(L'"');
	out += to_wide(p_executable);
	out.push_back(L'"');
	for (int i = 0; i < p_arguments.size(); ++i) {
		out.push_back(L' ');
		const std::wstring arg = to_wide(p_arguments[i]);
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

Dictionary SandboxingWin::spawn_target(const String &p_executable, const Vector<String> &p_arguments,
		const String &p_stdout_log_path) {
	Dictionary result;
	if (broker_service == nullptr) {
		ERR_PRINT("SandboxingWin::spawn_target called from a sandbox target process");
		return result;
	}

	if (!broker_initialized) {
		auto delegate = std::make_unique<BrokerServicesDelegateImpl>();
		sandbox::ResultCode init = broker_service->Init(std::move(delegate));
		if (init != sandbox::SBOX_ALL_OK) {
			ERR_PRINT(vformat("SandboxingWin: BrokerServices::Init failed (%d)", (int)init));
			return result;
		}
		broker_initialized = true;
	}

	std::unique_ptr<sandbox::TargetPolicy> policy = broker_service->CreatePolicy();
	sandbox::TargetConfig *config = policy->GetConfig();

	sandbox::ResultCode r;

	// Job restrictions: lockdown blocks children, clipboard, global hooks.
	r = config->SetJobLevel(sandbox::JobLevel::kLockdown, /*ui_exceptions=*/0);
	ERR_FAIL_COND_V_MSG(r != sandbox::SBOX_ALL_OK, result, vformat("SandboxingWin: SetJobLevel failed (%d)", (int)r));

	// Token: USER_LIMITED at lockdown is what Firefox's content process uses.
	// USER_LOCKDOWN is strictly tighter but breaks our IPC (renderer can't
	// open the launcher's named pipes) and the renderer's own log file under
	// %APPDATA% becomes unwritable. USER_LIMITED + Low integrity still
	// blocks USERPROFILE writes and HKCU registry writes — what we test for.
	r = config->SetTokenLevel(sandbox::TokenLevel::USER_RESTRICTED_SAME_ACCESS,
			sandbox::TokenLevel::USER_LIMITED);
	ERR_FAIL_COND_V_MSG(r != sandbox::SBOX_ALL_OK, result, vformat("SandboxingWin: SetTokenLevel failed (%d)", (int)r));

	// Alternate desktop blocks window messages from reaching the user's real
	// desktop. CreateAlternateDesktop is broker-global so calling it more than
	// once is fine — Chromium handles the dedupe.
	r = broker_service->CreateAlternateDesktop(sandbox::Desktop::kAlternateDesktop);
	if (r != sandbox::SBOX_ALL_OK && r != sandbox::SBOX_ERROR_GENERIC) {
		// Non-fatal: log and continue without alt-desktop rather than failing
		// the whole spawn.
		ERR_PRINT(vformat("SandboxingWin: CreateAlternateDesktop returned %d (continuing)", (int)r));
	}
	config->SetDesktop(sandbox::Desktop::kAlternateDesktop);

	// Delayed integrity: applied at LowerToken() time so initial bootstrap
	// has medium IL. UNTRUSTED is the strictest level — Chrome's renderer
	// runs here. The 4.3 prototype confirmed Vulkan rendering works at
	// UNTRUSTED on this hardware (per Implementation Status.md notes), so
	// we don't need to drop to LOW the way Firefox content processes do.
	config->SetDelayedIntegrityLevel(sandbox::IntegrityLevel::INTEGRITY_LEVEL_UNTRUSTED);

	// Allow the renderer to write its per-gate log file (and shader cache,
	// gates_data, etc.) under Godot's app_userdata dir. Chromium's file
	// policy pattern wildcards don't span path separators, so we need
	// multiple rules at different depths to cover the full tree we want
	// brokered.
	{
		wchar_t appdata[MAX_PATH] = {0};
		DWORD plen = ::GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
		if (plen > 0 && plen < MAX_PATH) {
			const std::wstring root = std::wstring(appdata) + L"\\Godot\\app_userdata\\TheGates";
			const wchar_t *globs[] = {
				L"\\*",
				L"\\*\\*",
				L"\\*\\*\\*",
				L"\\*\\*\\*\\*",
				L"\\*\\*\\*\\*\\*",
				L"\\*\\*\\*\\*\\*\\*",
			};
			for (const wchar_t *suffix : globs) {
				std::wstring pattern = root + suffix;
				sandbox::ResultCode rf = config->AllowFileAccess(
						sandbox::FileSemantics::kAllowAny, pattern.c_str());
				if (rf != sandbox::SBOX_ALL_OK) {
					ERR_PRINT(vformat("SandboxingWin: AllowFileAccess(%s) returned %d",
							String::utf16((const char16_t *)pattern.c_str()), (int)rf));
				}
			}
		}
	}

	// Baseline process mitigations applied at process creation — ASLR + DEP
	// hardening, no inherited handles surprises.
	//
	// Deliberately NOT set:
	//   - MITIGATION_WIN32K_DISABLE: needs GDI brokering, renderer does
	//     win32k syscalls during Vulkan + windowing init.
	//   - MITIGATION_STRICT_HANDLE_CHECKS: drives AMD/NVIDIA UMD double-
	//     CloseHandle / dangling-handle bugs to crash the renderer (per
	//     the research subagent run during the renderer build). Chromium's
	//     renderer never calls Vulkan; ours does.
	r = config->SetProcessMitigations(
			sandbox::MITIGATION_DEP |
			sandbox::MITIGATION_DEP_NO_ATL_THUNK |
			sandbox::MITIGATION_SEHOP |
			sandbox::MITIGATION_BOTTOM_UP_ASLR |
			sandbox::MITIGATION_HIGH_ENTROPY_ASLR);
	if (r != sandbox::SBOX_ALL_OK) {
		ERR_PRINT(vformat("SandboxingWin: SetProcessMitigations returned %d", (int)r));
	}

	r = config->SetDelayedProcessMitigations(
			sandbox::MITIGATION_DLL_SEARCH_ORDER);
	if (r != sandbox::SBOX_ALL_OK) {
		ERR_PRINT(vformat("SandboxingWin: SetDelayedProcessMitigations returned %d", (int)r));
	}

	const std::wstring exe_wide = to_wide(p_executable);
	const std::wstring cmd_wide = build_command_line(p_executable, p_arguments);

	// Pre-open the renderer's stdout/stderr log file at broker integrity so
	// the locked-down child has a writeable handle to its own log even after
	// dropping to LOW. Without this, sandbox-spawned children write nowhere
	// (they don't inherit the launcher's console handles).
	HANDLE log_handle = INVALID_HANDLE_VALUE;
	if (!p_stdout_log_path.is_empty()) {
		const std::wstring log_path_wide = to_wide(p_stdout_log_path);
		// Ensure parent dirs exist. SHCreateDirectoryExW would be ideal but
		// we keep this minimal — Godot will have already mkdir'd via
		// DirAccess from the GDScript wrapper.
		SECURITY_ATTRIBUTES sa = {};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		log_handle = ::CreateFileW(
				log_path_wide.c_str(),
				FILE_GENERIC_WRITE | FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				&sa,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
		if (log_handle == INVALID_HANDLE_VALUE) {
			ERR_PRINT(vformat("SandboxingWin: CreateFile(log) failed win=%d", (int)::GetLastError()));
		} else {
			sandbox::ResultCode rs = policy->SetStdoutHandle(log_handle);
			if (rs != sandbox::SBOX_ALL_OK) {
				ERR_PRINT(vformat("SandboxingWin: SetStdoutHandle returned %d", (int)rs));
			}
			rs = policy->SetStderrHandle(log_handle);
			if (rs != sandbox::SBOX_ALL_OK) {
				ERR_PRINT(vformat("SandboxingWin: SetStderrHandle returned %d", (int)rs));
			}
		}
	}

	DWORD last_error = 0;
	PROCESS_INFORMATION pi = {};
	sandbox::ResultCode spawn = broker_service->SpawnTarget(
			exe_wide.c_str(), cmd_wide.c_str(), std::move(policy), &last_error, &pi);
	if (spawn != sandbox::SBOX_ALL_OK) {
		if (log_handle != INVALID_HANDLE_VALUE) {
			::CloseHandle(log_handle);
		}
		ERR_PRINT(vformat("SandboxingWin: SpawnTarget failed (sbox=%d, win=%d)",
				(int)spawn, (int)last_error));
		return result;
	}

	// We can release our broker-side copy of the log handle now — the child
	// inherited its own dup and will keep the file open until it exits.
	if (log_handle != INVALID_HANDLE_VALUE) {
		::CloseHandle(log_handle);
	}

	// Resume the initial thread — SpawnTarget creates suspended.
	if (pi.hThread) {
		::ResumeThread(pi.hThread);
	}

	// Make the child visible to OS.is_process_running / OS.kill. Without this
	// the launcher's process_checker fires a spurious "Gate crashed on bootup".
	static_cast<OS_Windows *>(OS::get_singleton())->track_external_process(
			(OS::ProcessID)pi.dwProcessId, pi.hProcess, pi.hThread);

	result["pid"] = (int64_t)pi.dwProcessId;
	result["thread_id"] = (int64_t)pi.dwThreadId;
	result["process_handle"] = (int64_t)(uintptr_t)pi.hProcess;
	result["thread_handle"] = (int64_t)(uintptr_t)pi.hThread;
	return result;
}

Error SandboxingWin::lower_token() {
	ERR_FAIL_COND_V_MSG(!is_target(), ERR_UNAVAILABLE,
			"SandboxingWin: lower_token called from broker (not a sandbox target)");

	sandbox::TargetServices *target_service = sandbox::SandboxFactory::GetTargetServices();
	ERR_FAIL_COND_V_MSG(target_service == nullptr, FAILED,
			"SandboxingWin: GetTargetServices returned null");

	sandbox::ResultCode init = target_service->Init();
	ERR_FAIL_COND_V_MSG(init != sandbox::SBOX_ALL_OK, FAILED,
			vformat("SandboxingWin: TargetServices::Init failed (%d)", (int)init));

	target_service->LowerToken();
	print_line("SandboxingWin: LowerToken complete; renderer is now sandboxed");
	return OK;
}

void SandboxingWin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("spawn_target", "executable", "arguments", "stdout_log_path"),
			&SandboxingWin::spawn_target, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("lower_token"), &SandboxingWin::lower_token);
	ClassDB::bind_method(D_METHOD("is_target"), &SandboxingWin::is_target);
}

bool SandboxingWin::broker_initialized = false;

SandboxingWin::SandboxingWin() {
	broker_service = sandbox::SandboxFactory::GetBrokerServices();
}

SandboxingWin::~SandboxingWin() {
}
