/**************************************************************************/
/*  sandbox_diagnostics.cpp                                               */
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

#include "sandbox_diagnostics.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>

#include <fileapi.h>
#include <processthreadsapi.h>
#include <sddl.h>
#include <securitybaseapi.h>
#include <winternl.h>
#endif

#ifdef LINUXBSD_ENABLED
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef MACOS_ENABLED
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" int sandbox_check(pid_t pid, const char *operation, int type, ...);
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

	// Restricted SIDs count
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
	// Payload restriction
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
	wchar_t name[256] = { 0 };
	DWORD needed = 0;
	GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), &needed);
	return String::utf16((const char16_t *)name);
}

Dictionary run_canaries(const String &p_pack_path) {
	Dictionary out;

	// USERPROFILE write canary: a sandboxed renderer must not be able to
	// write here.
	{
		wchar_t profile[MAX_PATH] = { 0 };
		DWORD plen = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
		if (plen > 0 && plen < MAX_PATH) {
			String path = String::utf16((const char16_t *)profile) + "\\thegates-sandbox-canary.txt";
			HANDLE h = CreateFileW((LPCWSTR)path.utf16().get_data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h != INVALID_HANDLE_VALUE) {
				out["canary_file_write"] = "allowed";
				CloseHandle(h);
				DeleteFileW((LPCWSTR)path.utf16().get_data());
			} else {
				out["canary_file_write"] = "blocked";
				out["canary_file_write_error"] = (int)GetLastError();
			}
		} else {
			out["canary_file_write"] = "skipped_no_userprofile";
		}
	}

	// HKCU registry write canary.
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

	// Positive canary: write under the renderer's own user_data_dir. Gates
	// hit this path with FileAccess.open("user://...", WRITE).
	{
		Error err = OK;
		Ref<FileAccess> f = FileAccess::open("user://sandbox-positive-canary.txt", FileAccess::WRITE, &err);
		if (f.is_valid()) {
			f->store_string(vformat("pid=%d", (int)GetCurrentProcessId()));
			f->close();
			out["canary_user_dir_write"] = "allowed";
		} else {
			out["canary_user_dir_write"] = "blocked";
			out["canary_user_dir_write_error"] = (int)err;
		}
	}

	// Isolation canary: a sibling gate's folder must NOT be writable.
	{
		const String sibling = OS::get_singleton()->get_user_data_dir().path_join("..").path_join("sandbox-isolation-canary.txt");
		Error err = OK;
		Ref<FileAccess> f = FileAccess::open(sibling, FileAccess::WRITE, &err);
		if (f.is_valid()) {
			f->store_string(vformat("pid=%d", (int)GetCurrentProcessId()));
			f->close();
			out["canary_sibling_gate_write"] = "allowed";
		} else {
			out["canary_sibling_gate_write"] = "blocked";
			out["canary_sibling_gate_write_error"] = (int)err;
		}
	}

	// .pck read canary: the path Godot's ZIP reader hits on every load().
	{
		out["canary_pck_read_path"] = p_pack_path;
		if (!p_pack_path.is_empty()) {
			Error err = OK;
			Ref<FileAccess> f = FileAccess::open(p_pack_path, FileAccess::READ, &err);
			if (f.is_valid()) {
				out["canary_pck_read"] = "allowed";
			} else {
				out["canary_pck_read"] = "blocked";
				out["canary_pck_read_error"] = (int)err;
			}
		} else {
			out["canary_pck_read"] = "skipped_no_pck_path";
		}
	}

	return out;
}

#endif // WINDOWS_ENABLED

#ifdef LINUXBSD_ENABLED

int read_landlock_abi() {
	const long r = ::syscall(__NR_landlock_create_ruleset,
			(void *)nullptr, (size_t)0,
			(uint32_t)LANDLOCK_CREATE_RULESET_VERSION);
	if (r < 0) {
		return 0;
	}
	return (int)r;
}

String read_first_line(const char *p_path) {
	const int fd = ::open(p_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return String();
	}
	char buf[256] = { 0 };
	const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
	::close(fd);
	if (n <= 0) {
		return String();
	}
	String s = String::utf8(buf, (int)n);
	const int nl = s.find("\n");
	if (nl >= 0) {
		s = s.substr(0, nl);
	}
	return s.strip_edges();
}

// The default `0 0 4294967295` mapping is what every process inherits from
// the initial user namespace; anything narrower is a real namespace bounded
// by our broker. See user_namespaces(7).
bool detect_userns_active() {
	const String line = read_first_line("/proc/self/uid_map");
	if (line.is_empty()) {
		return false;
	}
	const PackedStringArray parts = line.split(" ", false);
	if (parts.size() < 3) {
		return false;
	}
	if (parts[0] == "0" && parts[1] == "0" && parts[2] == "4294967295") {
		return false;
	}
	return true;
}

// CapEff from /proc/self/status is the effective capability mask; zero means
// we hold no privileged capabilities. Returns UINT64_MAX on parse failure so
// the diagnostic shows "unknown" rather than a false-zero.
uint64_t read_cap_eff() {
	const int fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return UINT64_MAX;
	}
	char buf[4096] = { 0 };
	const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
	::close(fd);
	if (n <= 0) {
		return UINT64_MAX;
	}
	String contents = String::utf8(buf, (int)n);
	const int idx = contents.find("CapEff:\t");
	if (idx < 0) {
		return UINT64_MAX;
	}
	String hex = contents.substr(idx + 8);
	const int nl = hex.find("\n");
	if (nl >= 0) {
		hex = hex.substr(0, nl);
	}
	return (uint64_t)hex.hex_to_int();
}

String seccomp_mode_label(int p_mode) {
	switch (p_mode) {
		case SECCOMP_MODE_DISABLED:
			return "disabled";
		case SECCOMP_MODE_STRICT:
			return "strict";
		case SECCOMP_MODE_FILTER:
			return "filter";
		default:
			return "unknown";
	}
}

Dictionary run_canaries_linux(const String &p_pack_path) {
	Dictionary out;

	// Filesystem write to a path outside the policy allow-list. /etc/ is
	// world-readable but writes require root; the landlock + caps drops
	// should make this fail with EACCES even if we briefly held write rights.
	{
		const String path = "/etc/thegates-sandbox-canary";
		const int fd = ::open(path.utf8().get_data(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0) {
			out["canary_etc_write"] = "allowed";
			::close(fd);
			::unlink(path.utf8().get_data());
		} else {
			out["canary_etc_write"] = "blocked";
			out["canary_etc_write_error"] = (int)errno;
		}
	}

	// Home-dir write canary: the sandbox should keep us out of $HOME.
	{
		const char *home = ::getenv("HOME");
		if (home != nullptr && home[0] != '\0') {
			const String path = String::utf8(home) + "/.thegates-sandbox-canary";
			const int fd = ::open(path.utf8().get_data(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
			if (fd >= 0) {
				out["canary_file_write"] = "allowed";
				::close(fd);
				::unlink(path.utf8().get_data());
			} else {
				out["canary_file_write"] = "blocked";
				out["canary_file_write_error"] = (int)errno;
			}
		} else {
			out["canary_file_write"] = "skipped_no_home";
		}
	}

	// Positive canary: writing under the renderer's user_data_dir must succeed
	// once the per-gate landlock rule is in place.
	{
		Error err = OK;
		Ref<FileAccess> f = FileAccess::open("user://sandbox-positive-canary.txt", FileAccess::WRITE, &err);
		if (f.is_valid()) {
			f->store_string(vformat("pid=%d", (int)::getpid()));
			f->close();
			out["canary_user_dir_write"] = "allowed";
		} else {
			out["canary_user_dir_write"] = "blocked";
			out["canary_user_dir_write_error"] = (int)err;
		}
	}

	// Isolation canary: a sibling gate's folder must not be writable.
	{
		const String sibling = OS::get_singleton()->get_user_data_dir().path_join("..").path_join("sandbox-isolation-canary.txt");
		Error err = OK;
		Ref<FileAccess> f = FileAccess::open(sibling, FileAccess::WRITE, &err);
		if (f.is_valid()) {
			f->store_string(vformat("pid=%d", (int)::getpid()));
			f->close();
			out["canary_sibling_gate_write"] = "allowed";
		} else {
			out["canary_sibling_gate_write"] = "blocked";
			out["canary_sibling_gate_write_error"] = (int)err;
		}
	}

	// Network canary: a non-loopback TCP connect must be blocked.
	{
		const int sock = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (sock < 0) {
			out["canary_network"] = "blocked";
			out["canary_network_error"] = (int)errno;
		} else {
			sockaddr_in addr = {};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(80);
			addr.sin_addr.s_addr = htonl(0x08080808u); // 8.8.8.8
			const int rc = ::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
			if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
				out["canary_network"] = "allowed";
			} else {
				out["canary_network"] = "blocked";
				out["canary_network_error"] = (int)errno;
			}
			::close(sock);
		}
	}

	// .pck read canary: load() re-opens the pack on every resource fetch.
	{
		out["canary_pck_read_path"] = p_pack_path;
		if (!p_pack_path.is_empty()) {
			Error err = OK;
			Ref<FileAccess> f = FileAccess::open(p_pack_path, FileAccess::READ, &err);
			if (f.is_valid()) {
				out["canary_pck_read"] = "allowed";
			} else {
				out["canary_pck_read"] = "blocked";
				out["canary_pck_read_error"] = (int)err;
			}
		} else {
			out["canary_pck_read"] = "skipped_no_pck_path";
		}
	}

	return out;
}

#endif // LINUXBSD_ENABLED

#ifdef MACOS_ENABLED

Dictionary run_canaries_macos(const String &p_pack_path) {
	Dictionary out;

	// /etc/ write should be blocked by both POSIX perms and the sandbox.
	{
		const String path = "/etc/thegates-sandbox-canary";
		const int fd = ::open(path.utf8().get_data(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0) {
			out["canary_etc_write"] = "allowed";
			::close(fd);
			::unlink(path.utf8().get_data());
		} else {
			out["canary_etc_write"] = "blocked";
			out["canary_etc_write_error"] = (int)errno;
		}
	}

	// $HOME write should be blocked by the Seatbelt profile.
	{
		const char *home = ::getenv("HOME");
		if (home != nullptr && home[0] != '\0') {
			const String path = String::utf8(home) + "/.thegates-sandbox-canary";
			const int fd = ::open(path.utf8().get_data(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
			if (fd >= 0) {
				out["canary_file_write"] = "allowed";
				::close(fd);
				::unlink(path.utf8().get_data());
			} else {
				out["canary_file_write"] = "blocked";
				out["canary_file_write_error"] = (int)errno;
			}
		} else {
			out["canary_file_write"] = "skipped_no_home";
		}
	}

	// Positive canary: writing under user:// (the per-gate dir) must succeed.
	{
		Error err = OK;
		Ref<FileAccess> f = FileAccess::open("user://sandbox-positive-canary.txt", FileAccess::WRITE, &err);
		if (f.is_valid()) {
			f->store_string(vformat("pid=%d", (int)::getpid()));
			f->close();
			out["canary_user_dir_write"] = "allowed";
		} else {
			out["canary_user_dir_write"] = "blocked";
			out["canary_user_dir_write_error"] = (int)err;
		}
	}

	// Isolation canary: a sibling gate's folder must not be writable.
	{
		const String sibling = OS::get_singleton()->get_user_data_dir().path_join("..").path_join("sandbox-isolation-canary.txt");
		Error err = OK;
		Ref<FileAccess> f = FileAccess::open(sibling, FileAccess::WRITE, &err);
		if (f.is_valid()) {
			f->store_string(vformat("pid=%d", (int)::getpid()));
			f->close();
			out["canary_sibling_gate_write"] = "allowed";
		} else {
			out["canary_sibling_gate_write"] = "blocked";
			out["canary_sibling_gate_write_error"] = (int)err;
		}
	}

	// Network canary: a non-loopback TCP connect — blocked when seccomp denies
	// socket creation, allowed otherwise. Non-blocking so the canary doesn't
	// stall on real network latency to 8.8.8.8.
	{
		const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0) {
			out["canary_network"] = "blocked";
			out["canary_network_error"] = (int)errno;
		} else {
			int flags = ::fcntl(sock, F_GETFL, 0);
			if (flags >= 0) {
				::fcntl(sock, F_SETFL, flags | O_NONBLOCK);
			}
			sockaddr_in addr = {};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(80);
			addr.sin_addr.s_addr = htonl(0x08080808u); // 8.8.8.8
			const int rc = ::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
			if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
				out["canary_network"] = "allowed";
			} else {
				out["canary_network"] = "blocked";
				out["canary_network_error"] = (int)errno;
			}
			::close(sock);
		}
	}

	// .pck read canary: load() re-opens the pack on every resource fetch.
	{
		out["canary_pck_read_path"] = p_pack_path;
		if (!p_pack_path.is_empty()) {
			Error err = OK;
			Ref<FileAccess> f = FileAccess::open(p_pack_path, FileAccess::READ, &err);
			if (f.is_valid()) {
				out["canary_pck_read"] = "allowed";
			} else {
				out["canary_pck_read"] = "blocked";
				out["canary_pck_read_error"] = (int)err;
			}
		} else {
			out["canary_pck_read"] = "skipped_no_pck_path";
		}
	}

	return out;
}

#endif // MACOS_ENABLED

} // namespace

Dictionary SandboxDiagnostics::to_dict() const {
	Dictionary diag;

#ifdef WINDOWS_ENABLED
	diag["platform"] = "windows";
	diag["pid"] = (int)GetCurrentProcessId();
	diag["build"] = String(
#ifdef TG_SANDBOX
			"tg_sandbox=yes"
#else
			"tg_sandbox=no"
#endif
	);

	Dictionary token = collect_token_info();
	for (const Variant *k = token.next(nullptr); k; k = token.next(k)) {
		diag[*k] = token[*k];
	}

	diag["mitigations"] = collect_mitigations();
	diag["alt_desktop"] = current_desktop_name();
	diag["canaries"] = run_canaries(pack_path);
#elif defined(LINUXBSD_ENABLED)
	diag["platform"] = "linux";
	diag["pid"] = (int)::getpid();
	diag["build"] = String(
#ifdef TG_SANDBOX
			"tg_sandbox=yes"
#else
			"tg_sandbox=no"
#endif
	);

	const int seccomp_mode = ::prctl(PR_GET_SECCOMP);
	diag["seccomp_mode"] = seccomp_mode;
	diag["seccomp"] = seccomp_mode_label(seccomp_mode);
	diag["no_new_privs"] = ::prctl(PR_GET_NO_NEW_PRIVS) == 1;
	diag["landlock_abi"] = read_landlock_abi();
	diag["userns_active"] = detect_userns_active();

	const uint64_t cap_eff = read_cap_eff();
	if (cap_eff == UINT64_MAX) {
		diag["cap_eff_hex"] = "unknown";
		diag["cap_eff_zero"] = false;
	} else {
		diag["cap_eff_hex"] = String::num_uint64(cap_eff, 16).pad_zeros(16);
		diag["cap_eff_zero"] = cap_eff == 0;
	}

	// Token-equivalent on Linux: untrusted when capabilities are empty and a
	// seccomp filter is installed (which itself implies PR_SET_NO_NEW_PRIVS
	// since the kernel rejects PR_SET_SECCOMP without it).
	diag["integrity"] = (cap_eff == 0 && seccomp_mode == SECCOMP_MODE_FILTER) ? "untrusted" : "low";

	diag["canaries"] = run_canaries_linux(pack_path);
#elif defined(MACOS_ENABLED)
	diag["platform"] = "macos";
	diag["pid"] = (int)::getpid();
	diag["build"] = String(
#ifdef TG_SANDBOX
			"tg_sandbox=yes"
#else
			"tg_sandbox=no"
#endif
	);

	const bool sandbox_active = ::sandbox_check(::getpid(), nullptr, 0) == 1;
	diag["sandbox_active"] = sandbox_active;
	diag["integrity"] = sandbox_active ? "untrusted" : "unsandboxed";

	diag["canaries"] = run_canaries_macos(pack_path);
#else
	diag["platform"] = "unknown";
#endif

	return diag;
}

String SandboxDiagnostics::to_json_block() const {
	const String json = JSON::stringify(to_dict());
	return "=== SANDBOX-DIAG-BEGIN ===\n" + json + "\n=== SANDBOX-DIAG-END ===";
}

Error SandboxDiagnostics::write_verify_file(const String &p_path) const {
	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
	if (file.is_null()) {
		return err != OK ? err : ERR_CANT_OPEN;
	}
	file->store_string(JSON::stringify(to_dict(), "\t", false));
	return OK;
}
