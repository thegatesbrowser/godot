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
#include "broker_delegate.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/worker_thread_pool.h"
#include "core/string/print_string.h"
#include "core/templates/hash_set.h"
#include "handle_scope.h"
#include "signature_verify.h"
#include "string_utils.h"
#include "win_acl.h"

#include <atomic>
#include <memory>
#include <string>

#include <sandbox/win/src/sandbox.h>
#include <sandbox/win/src/sandbox_factory.h>
#include <sandbox/win/src/sandbox_policy.h>
#include <sandbox/win/src/security_level.h>

#include <windows.h>

#include <aclapi.h>

namespace {

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

// Duplex byte-mode named pipe pair; child end is inheritable. Handed to the
// renderer via TargetPolicy::AddHandleToShare, which routes it through
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST so the child inherits this handle and
// nothing else.
bool create_broker_pipe_pair(HANDLE *r_launcher_pipe, HANDLE *r_child_pipe) {
	static std::atomic<uint32_t> counter{ 0 };
	const uint32_t serial = counter.fetch_add(1, std::memory_order_relaxed);
	wchar_t name_buf[128] = {};
	::swprintf_s(name_buf, L"\\\\.\\pipe\\tg-broker-%lu-%lu",
			(unsigned long)::GetCurrentProcessId(), (unsigned long)serial);

	const HANDLE launcher_pipe = ::CreateNamedPipeW(
			name_buf,
			PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS | PIPE_WAIT,
			1, 65536, 65536, 0, nullptr);
	if (launcher_pipe == INVALID_HANDLE_VALUE) {
		return false;
	}

	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	const HANDLE child_pipe = ::CreateFileW(
			name_buf,
			GENERIC_READ | GENERIC_WRITE,
			0,
			&sa,
			OPEN_EXISTING,
			SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
			nullptr);
	if (child_pipe == INVALID_HANDLE_VALUE) {
		::CloseHandle(launcher_pipe);
		return false;
	}

	*r_launcher_pipe = launcher_pipe;
	*r_child_pipe = child_pipe;
	return true;
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

	// Clean up any IPC socket files an earlier renderer may have left bound.
	// In multi-gate cycles, the new renderer is spawned BEFORE the old one
	// is killed; without this pre-spawn sweep, the new (AppContainer)
	// renderer can't unlink the previous AppContainer's leftover file and
	// zmq bind() hangs. Launcher has full privilege; renderer (AC) does not.
	const PackedStringArray bound_files = p_policy->get_renderer_bound_files();
	for (int i = 0; i < bound_files.size(); ++i) {
		const Char16String utf16 = bound_files[i].replace("/", "\\").utf16();
		::DeleteFileW((const wchar_t *)utf16.get_data());
	}

	// Network isolation is enforced by the in-process NetworkBroker plus
	// the AppContainer SID/WFP combo. Control channel is a duplex byte-mode
	// named pipe whose child end inherits via TargetPolicy::AddHandleToShare
	// (routed through PROC_THREAD_ATTRIBUTE_HANDLE_LIST so the child gets
	// exactly this handle, nothing else). Numeric value travels as
	// --tg-broker-fd=<intptr> on the command line.
	HANDLE launcher_pipe = INVALID_HANDLE_VALUE;
	HANDLE child_pipe = INVALID_HANDLE_VALUE;
	if (!create_broker_pipe_pair(&launcher_pipe, &child_pipe)) {
		ERR_PRINT(vformat("SandboxWin: create_broker_pipe_pair failed (win=%d)", (int)::GetLastError()));
		return result;
	}
	HandleScope launcher_pipe_scope(launcher_pipe);
	HandleScope child_pipe_scope(child_pipe);

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

	// USER_LOCKDOWN's restricting-SID set {Null Sid} fails the windowing
	// subsystem's access check (CreateWindow / vkCreateInstance), so we use
	// USER_LIMITED for Vulkan + IPC. AF_INET denial comes from the
	// AppContainer profile added below, not the token level.
	r = config->SetTokenLevel(sandbox::TokenLevel::USER_RESTRICTED_SAME_ACCESS,
			sandbox::TokenLevel::USER_LIMITED);
	ERR_FAIL_COND_V_MSG(r != sandbox::SBOX_ALL_OK, result, vformat("SandboxWin: SetTokenLevel failed (%d)", (int)r));

	r = broker_service->CreateAlternateDesktop(sandbox::Desktop::kAlternateDesktop);
	if (r != sandbox::SBOX_ALL_OK && r != sandbox::SBOX_ERROR_GENERIC) {
		ERR_PRINT(vformat("SandboxWin: CreateAlternateDesktop returned %d (continuing)", (int)r));
	}
	config->SetDesktop(sandbox::Desktop::kAlternateDesktop);

	config->SetDelayedIntegrityLevel(sandbox::IntegrityLevel::INTEGRITY_LEVEL_UNTRUSTED);

	// AppContainer profile registers a per-gate SID with the kernel and creates
	// the package profile dir under %LOCALAPPDATA%\Packages. WFP then blocks
	// every AF_INET connect() from the renderer at ALE_AUTH_CONNECT (no
	// internetClient capability granted). Inherited broker sockets still work
	// because AFD captures the launcher's security context at socket creation.
	// Package name is a stable hash of rw_dir, computed identically here and in
	// apply_renderer_acl so the per-gate dir's SID grant matches this profile.
	const String pergate_pkg = WinACL::pergate_package_name(rw_dir);
	const Char16String pkg_utf16 = pergate_pkg.utf16();
	r = config->AddAppContainerProfile((const wchar_t *)pkg_utf16.get_data());
	if (r != sandbox::SBOX_ALL_OK) {
		ERR_PRINT(vformat("SandboxWin: AddAppContainerProfile(%s) failed (%d)",
				pergate_pkg, (int)r));
		return result;
	}
	PSID pergate_sid = WinACL::alloc_pergate_sid(pergate_pkg);
	if (pergate_sid == nullptr) {
		return result;
	}

	// Chromium's policy matcher uses NT-style paths (backslashes); single-asterisk globs
	// don't cross path separators, so a dir needs one rule per depth level.
	auto path_to_wstring = [](const String &p_path) {
		const String normalized = p_path.replace("/", "\\");
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
	HashSet<String> ipc_parent_dirs;
	for (int i = 0; i < rw_files.size(); ++i) {
		allow_file(sandbox::FileSemantics::kAllowAny, path_to_wstring(rw_files[i]));
		const String parent = rw_files[i].get_base_dir();
		if (!parent.is_empty()) {
			ipc_parent_dirs.insert(parent);
		}
	}
	// The IPC parent dir is shared across every gate's renderer (the launcher's
	// scratch dir holds command_sync, input_sync, external_texture for every
	// gate), so we grant All Application Packages (S-1-15-2-1) on it, not the
	// per-gate SID. Dispatched fire-and-forget to a worker thread because a
	// real-time AV filter driver intercepts SetSecurityInfo and scans the dir
	// tree — up to ~2s on cold cache. We don't block the launcher's main
	// thread on that. The WinACL static cache makes second+ cycles a no-op.
	//
	// First-launch race: on the very first invocation on a fresh machine, the
	// renderer may reach its first IPC connect() before this grant lands. In
	// practice the SetSecurityInfo also persists on disk, so subsequent
	// launches see the grant already in place and the renderer connects fine.
	PSID all_pkgs_sid = WinACL::alloc_all_packages_sid();
	if (all_pkgs_sid != nullptr) {
		struct GrantTask {
			PSID sid = nullptr;
			PackedStringArray parents;
		};
		GrantTask *task = memnew(GrantTask);
		for (const String &parent : ipc_parent_dirs) {
			task->parents.push_back(parent);
		}
		const DWORD sid_len = ::GetLengthSid(all_pkgs_sid);
		task->sid = (PSID)::LocalAlloc(LPTR, sid_len);
		::CopySid(sid_len, task->sid, all_pkgs_sid);
		::FreeSid(all_pkgs_sid);
		WorkerThreadPool::get_singleton()->add_native_task(
				[](void *p_ud) {
					GrantTask *t = static_cast<GrantTask *>(p_ud);
					WinACL acl;
					const ACCESS_MASK dir_mask = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
							FILE_TRAVERSE | FILE_LIST_DIRECTORY;
					for (int i = 0; i < t->parents.size(); ++i) {
						acl.grant_sid_access(t->sid, t->parents[i], dir_mask, NO_INHERITANCE);
					}
					::LocalFree(t->sid);
					memdelete(t);
				},
				task, false, "TheGates: AppContainer IPC dir ACL");
	}
	for (int i = 0; i < ro_files.size(); ++i) {
		const String &ro_path = ro_files[i];
		const DWORD attrs = ::GetFileAttributesW((const wchar_t *)ro_path.utf16().get_data());
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			allow_dir_recursive(sandbox::FileSemantics::kAllowReadonly, ro_path);
		} else {
			allow_file(sandbox::FileSemantics::kAllowReadonly, path_to_wstring(ro_path));
		}
		// AppContainer access check is per-file with NO_INHERITANCE — granting
		// recursively on a large shared_libs tree blocks Godot startup for
		// seconds (each leaf is a separate SetSecurityInfo). Cached per-path.
		win_acl.grant_sid_access(pergate_sid, ro_path,
				GENERIC_READ | GENERIC_EXECUTE, NO_INHERITANCE);
	}

	::FreeSid(pergate_sid);

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
	// Append --tg-broker-fd=<numeric handle> so the renderer reads it from
	// argv at engage_network_broker (replaces the older TG_BROKER_FD env var
	// which required a process-wide mutex around SetEnvironmentVariable +
	// SpawnTarget + restore). Godot's vformat %d handles int64 natively.
	Vector<String> args_with_broker = p_arguments;
	args_with_broker.push_back(vformat("--tg-broker-fd=%d",
			(int64_t)(intptr_t)child_pipe));
	const std::wstring cmd_wide = build_command_line(p_executable, args_with_broker);

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

	// AddHandleToShare flips HANDLE_FLAG_INHERIT and queues the handle for
	// PROC_THREAD_ATTRIBUTE_HANDLE_LIST, so the child inherits exactly this
	// handle and nothing else.
	policy->AddHandleToShare(child_pipe);

	DWORD last_error = 0;
	PROCESS_INFORMATION pi = {};
	const sandbox::ResultCode spawn = broker_service->SpawnTarget(
			exe_wide.c_str(), cmd_wide.c_str(), std::move(policy), &last_error, &pi);
	if (spawn != sandbox::SBOX_ALL_OK) {
		ERR_PRINT(vformat("SandboxWin: SpawnTarget failed (sbox=%d, win=%d)",
				(int)spawn, (int)last_error));
		return result;
	}

	// Launcher's local copy of the child end is no longer needed — the
	// renderer holds its own kernel reference via inheritance.
	child_pipe_scope.reset();

	// SpawnTarget creates the child suspended.
	if (pi.hThread) {
		::ResumeThread(pi.hThread);
	}

	target_process.reset(pi.hProcess);
	target_thread.reset(pi.hThread);
	target_pid = (int64_t)pi.dwProcessId;

	// Snapshot renderer-bound files for kill_target's cleanup pass. Policy is
	// the source of truth — the launcher (GDScript) tags which rw_files the
	// renderer binds, so no magic-string filter lives here.
	bound_files_to_clean = bound_files;

	// NetworkBroker owns the launcher half from here; release before start.
	start_broker((intptr_t)launcher_pipe_scope.release(), (void *)target_process.get());

	result["pid"] = (int64_t)pi.dwProcessId;
	result["thread_id"] = (int64_t)pi.dwThreadId;

	if (!stdout_log_path.is_empty()) {
		write_broker_policy_json(stdout_log_path, p_executable, pi.dwProcessId, p_policy);
	}

	return result;
}

void SandboxWin::apply_renderer_acl(const String &p_path) {
	// Baseline label keeps the AC alias in place — the IPC scratch dir relies
	// on it for cross-renderer access. The per-gate SID grant on top is what
	// blocks sibling gates: each gate's renderer holds a different package SID
	// and the chromium-sandbox AllowFileAccess policy only lists THIS gate's
	// rw_dir, so no sibling per-gate dir is reachable through the broker proxy.
	win_acl.stamp_untrusted_label(p_path);
	const String pkg_name = WinACL::pergate_package_name(p_path);
	PSID pergate_sid = WinACL::alloc_pergate_sid(pkg_name);
	if (pergate_sid != nullptr) {
		win_acl.grant_sid_access(pergate_sid, p_path, GENERIC_ALL, NO_INHERITANCE);
		::FreeSid(pergate_sid);
	}
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
		stop_broker();
		return ERR_DOES_NOT_EXIST;
	}
	if (!::TerminateProcess(target_process.get(), 1)) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxWin::kill_target: TerminateProcess failed (win=%d)", (int)::GetLastError()));
	}
	stop_broker();

	// AppContainer renderers can't always unlink files they bound. Clean up
	// from the launcher (full-privilege) side. List comes from policy via
	// spawn_target's snapshot.
	for (const String &p : bound_files_to_clean) {
		const Char16String utf16 = p.replace("/", "\\").utf16();
		::DeleteFileW((const wchar_t *)utf16.get_data());
	}
	bound_files_to_clean.clear();
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

bool SandboxWin::broker_initialized = false;

SandboxWin::SandboxWin() {
	broker_service = sandbox::SandboxFactory::GetBrokerServices();
}
