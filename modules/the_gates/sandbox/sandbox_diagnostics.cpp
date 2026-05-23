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
#include <winsock2.h>
#include <ws2tcpip.h>
// windows.h after winsock2.h to avoid winsock.h conflicts.
#include <windows.h>

#include <fileapi.h>
#include <processthreadsapi.h>
#include <sddl.h>
#include <securitybaseapi.h>
#include <winternl.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef LINUXBSD_ENABLED
#include "linux/lockdown.h"

#include "core/io/net_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/netlink.h>
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

// All canaries return a {status, error?} Dictionary for shape uniformity. The
// harness asserts on .status; .error is a platform errno for debugging blocked
// outcomes and is omitted when zero.
Dictionary make_canary(const String &p_status, int p_error = 0) {
	Dictionary d;
	d["status"] = p_status;
	if (p_error != 0) {
		d["error"] = p_error;
	}
	return d;
}

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
				out["canary_file_write"] = make_canary("allowed");
				CloseHandle(h);
				DeleteFileW((LPCWSTR)path.utf16().get_data());
			} else {
				out["canary_file_write"] = make_canary("blocked", (int)GetLastError());
			}
		} else {
			out["canary_file_write"] = make_canary("skipped_no_userprofile");
		}
	}

	// HKCU registry write canary.
	{
		HKEY key = nullptr;
		LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_SET_VALUE, &key);
		if (res == ERROR_SUCCESS) {
			out["canary_reg_write"] = make_canary("allowed");
			RegCloseKey(key);
		} else {
			out["canary_reg_write"] = make_canary("blocked", (int)res);
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
			out["canary_user_dir_write"] = make_canary("allowed");
		} else {
			out["canary_user_dir_write"] = make_canary("blocked", (int)err);
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
			out["canary_sibling_gate_write"] = make_canary("allowed");
		} else {
			out["canary_sibling_gate_write"] = make_canary("blocked", (int)err);
		}
	}

	// .pck read canary: the path Godot's ZIP reader hits on every load().
	{
		out["canary_pck_read_path"] = p_pack_path;
		if (!p_pack_path.is_empty()) {
			Error err = OK;
			Ref<FileAccess> f = FileAccess::open(p_pack_path, FileAccess::READ, &err);
			if (f.is_valid()) {
				out["canary_pck_read"] = make_canary("allowed");
			} else {
				out["canary_pck_read"] = make_canary("blocked", (int)err);
			}
		} else {
			out["canary_pck_read"] = make_canary("skipped_no_pck_path");
		}
	}

	// Network canaries. With the FD-passing broker architecture, the
	// renderer's raw socket() syscall is denied (Seatbelt/seccomp/Windows
	// token); all three connect attempts must fail at socket creation.
	// The "private blocked / public allowed" distinction is verified by
	// the GDScript-level broker test in run-sandbox-test.py, which goes
	// through Godot's NetSocket (i.e. through the broker).
	{
		WSADATA wsa = {};
		const int wsa_rc = WSAStartup(MAKEWORD(2, 2), &wsa);

		auto try_connect = [&](uint32_t addr_be, uint16_t port_be) -> Dictionary {
			Dictionary r;
			if (wsa_rc != 0) {
				r["status"] = "blocked";
				r["error"] = wsa_rc;
				return r;
			}
			SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (sock == INVALID_SOCKET) {
				r["status"] = "blocked";
				r["error"] = (int)WSAGetLastError();
				return r;
			}
			// Non-blocking + select() with a 1.5s timeout. The USER_LIMITED
			// token blocks raw socket() creation, so this connect should
			// never get past socket(); we still do select() defensively in
			// case a future kernel/driver path routes the denial through
			// connect+SO_ERROR.
			u_long nb = 1;
			ioctlsocket(sock, FIONBIO, &nb);
			sockaddr_in dest = {};
			dest.sin_family = AF_INET;
			dest.sin_port = port_be;
			dest.sin_addr.s_addr = addr_be;
			const int rc = ::connect(sock, reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
			int last = WSAGetLastError();
			if (rc != 0 && last != WSAEWOULDBLOCK) {
				closesocket(sock);
				r["status"] = "blocked";
				r["error"] = last;
				return r;
			}
			fd_set wfds, efds;
			FD_ZERO(&wfds);
			FD_ZERO(&efds);
			FD_SET(sock, &wfds);
			FD_SET(sock, &efds);
			timeval tv = { 1, 500000 };
			const int sel = select(0, nullptr, &wfds, &efds, &tv);
			if (sel <= 0) {
				closesocket(sock);
				r["status"] = "blocked";
				r["error"] = sel == 0 ? -1 : (int)WSAGetLastError();
				return r;
			}
			int so_error = 0;
			int so_error_len = sizeof(so_error);
			if (FD_ISSET(sock, &efds) || (FD_ISSET(sock, &wfds) && getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &so_error_len) == 0 && so_error != 0)) {
				closesocket(sock);
				r["status"] = "blocked";
				r["error"] = so_error != 0 ? so_error : (int)WSAGetLastError();
				return r;
			}
			closesocket(sock);
			r["status"] = "allowed";
			return r;
		};

		// AppContainer (no INTERNET_CLIENT capability) makes WFP block all
		// AF_INET connect() attempts at the ALE_AUTH_CONNECT layer. We probe
		// public, private, and loopback — all four must be blocked for the
		// threat model to hold. Inherited broker sockets still work because
		// AFD captured the launcher's security context at socket creation.
		out["canary_raw_socket_denied"] = try_connect(htonl(0x01010101u), htons(443));
		out["canary_private_ip_blocked"] = try_connect(htonl(0xC0A80101u), htons(80));
		out["canary_localhost_blocked"] = try_connect(htonl(0x7F000001u), htons(22));
		out["canary_public_ip_allowed"] = try_connect(htonl(0x01010101u), htons(443));

		if (wsa_rc == 0) {
			WSACleanup();
		}
	}

	return out;
}

#endif // WINDOWS_ENABLED

#ifdef LINUXBSD_ENABLED

// Cached by lockdown.cpp before seccomp installs; querying landlock_create_ruleset
// here would return EPERM because seccomp's allowlist doesn't include it.
int read_landlock_abi() {
	return tg_lockdown_landlock_abi();
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
			out["canary_etc_write"] = make_canary("allowed");
			::close(fd);
			::unlink(path.utf8().get_data());
		} else {
			out["canary_etc_write"] = make_canary("blocked", (int)errno);
		}
	}

	// Home-dir write canary: the sandbox should keep us out of $HOME.
	{
		const char *home = ::getenv("HOME");
		if (home != nullptr && home[0] != '\0') {
			const String path = String::utf8(home) + "/.thegates-sandbox-canary";
			const int fd = ::open(path.utf8().get_data(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
			if (fd >= 0) {
				out["canary_file_write"] = make_canary("allowed");
				::close(fd);
				::unlink(path.utf8().get_data());
			} else {
				out["canary_file_write"] = make_canary("blocked", (int)errno);
			}
		} else {
			out["canary_file_write"] = make_canary("skipped_no_home");
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
			out["canary_user_dir_write"] = make_canary("allowed");
		} else {
			out["canary_user_dir_write"] = make_canary("blocked", (int)err);
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
			out["canary_sibling_gate_write"] = make_canary("allowed");
		} else {
			out["canary_sibling_gate_write"] = make_canary("blocked", (int)err);
		}
	}

	// Network canaries: with the FD-passing broker architecture the
	// renderer's raw socket() syscall is denied by seccomp; all three
	// connect attempts fail at socket creation (EPERM/EACCES). The
	// CIDR-policy distinction is verified at the broker level via the
	// GDScript test that uses Godot's NetSocket. See run-sandbox-test.py.
	{
		auto try_connect = [](uint32_t addr_be, uint16_t port_be) -> Dictionary {
			Dictionary r;
			const int sock = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
			if (sock < 0) {
				r["status"] = "blocked";
				r["error"] = (int)errno;
				return r;
			}
			sockaddr_in addr = {};
			addr.sin_family = AF_INET;
			addr.sin_port = port_be;
			addr.sin_addr.s_addr = addr_be;
			const int rc = ::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
			if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
				r["status"] = "allowed";
			} else {
				r["status"] = "blocked";
				r["error"] = (int)errno;
			}
			::close(sock);
			return r;
		};

		// All three are now blocked at socket() creation by seccomp.
		out["canary_raw_socket_denied"] = try_connect(htonl(0x01010101u), htons(443));
		out["canary_private_ip_blocked"] = try_connect(htonl(0xC0A80101u), htons(80));
		out["canary_localhost_blocked"] = try_connect(htonl(0x7F000001u), htons(22));
		out["canary_public_ip_allowed"] = try_connect(htonl(0x01010101u), htons(443));
	}

	// socketpair canary: seccomp denies __NR_socketpair (different syscall
	// from __NR_socket on x86_64). Existing FDs inherited from the launcher
	// survive; new ones cannot be created.
	{
		int sv[2] = { -1, -1 };
		const int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
		if (rc == 0) {
			out["canary_socketpair_denied"] = make_canary("allowed");
			::close(sv[0]);
			::close(sv[1]);
		} else {
			out["canary_socketpair_denied"] = make_canary("blocked", (int)errno);
		}
	}

	// AF_NETLINK canary: covered transitively by socket() denial, but worth
	// asserting explicitly because netlink would leak interface + route info
	// even without ever sending a packet.
	{
		const int fd = ::socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
		if (fd >= 0) {
			out["canary_netlink_denied"] = make_canary("allowed");
			::close(fd);
		} else {
			out["canary_netlink_denied"] = make_canary("blocked", (int)errno);
		}
	}

	// userfaultfd canary: recurring sandbox-escape vector, denied by both
	// Chromium and Firefox. We remove __NR_userfaultfd from the allowlist
	// so the syscall returns EPERM.
	{
		const long fd = ::syscall(__NR_userfaultfd, O_CLOEXEC);
		if (fd >= 0) {
			out["canary_userfaultfd_denied"] = make_canary("allowed");
			::close((int)fd);
		} else {
			out["canary_userfaultfd_denied"] = make_canary("blocked", (int)errno);
		}
	}

	// /proc/self/net canary: the symlinked /proc/net subtree exposes the
	// host's TCP/UDP socket tables. Landlock now skips /proc/self as a tree,
	// allowing only the specific /proc/self/<file> paths the renderer needs.
	{
		const int fd = ::open("/proc/self/net/tcp", O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			out["canary_proc_net_blocked"] = make_canary("allowed");
			::close(fd);
		} else {
			out["canary_proc_net_blocked"] = make_canary("blocked", (int)errno);
		}
	}

	// Brokered-connect canary: NetSocket::create() is BrokeredNetSocket here
	// (engage_network_broker installed it pre-lockdown). The canary exercises
	// the broker round-trip end-to-end. Default mode: broker connects to a
	// public IP and returns a connected FD -> "allowed". negative-broker mode
	// (TG_NETWORK_BROKER_FORCE_FAIL=1): broker denies the request, returns
	// ST_DENIED_FORCE_FAIL -> ERR_UNAUTHORIZED -> "blocked".
	{
		NetSocket *raw = NetSocket::create();
		if (raw == nullptr) {
			out["canary_brokered_connect"] = make_canary("skipped_no_factory");
		} else {
			Ref<NetSocket> sock(raw);
			IP::Type ip_type = IP::TYPE_IPV6;
			const Error open_err = sock->open(NetSocket::TYPE_TCP, ip_type);
			if (open_err != OK) {
				out["canary_brokered_connect"] = make_canary("skipped_open_failed", (int)open_err);
			} else {
				IPAddress dest("1.1.1.1");
				const Error e = sock->connect_to_host(dest, 443);
				if (e == OK || e == ERR_BUSY) {
					out["canary_brokered_connect"] = make_canary("allowed");
				} else if (e == ERR_UNAUTHORIZED) {
					out["canary_brokered_connect"] = make_canary("blocked", (int)e);
				} else {
					out["canary_brokered_connect"] = make_canary("blocked", (int)e);
				}
				sock->close();
			}
		}
	}

	// .pck read canary: load() re-opens the pack on every resource fetch.
	{
		out["canary_pck_read_path"] = p_pack_path;
		if (!p_pack_path.is_empty()) {
			Error err = OK;
			Ref<FileAccess> f = FileAccess::open(p_pack_path, FileAccess::READ, &err);
			if (f.is_valid()) {
				out["canary_pck_read"] = make_canary("allowed");
			} else {
				out["canary_pck_read"] = make_canary("blocked", (int)err);
			}
		} else {
			out["canary_pck_read"] = make_canary("skipped_no_pck_path");
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
			out["canary_etc_write"] = make_canary("allowed");
			::close(fd);
			::unlink(path.utf8().get_data());
		} else {
			out["canary_etc_write"] = make_canary("blocked", (int)errno);
		}
	}

	// $HOME write should be blocked by the Seatbelt profile.
	{
		const char *home = ::getenv("HOME");
		if (home != nullptr && home[0] != '\0') {
			const String path = String::utf8(home) + "/.thegates-sandbox-canary";
			const int fd = ::open(path.utf8().get_data(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
			if (fd >= 0) {
				out["canary_file_write"] = make_canary("allowed");
				::close(fd);
				::unlink(path.utf8().get_data());
			} else {
				out["canary_file_write"] = make_canary("blocked", (int)errno);
			}
		} else {
			out["canary_file_write"] = make_canary("skipped_no_home");
		}
	}

	// Positive canary: writing under user:// (the per-gate dir) must succeed.
	{
		Error err = OK;
		Ref<FileAccess> f = FileAccess::open("user://sandbox-positive-canary.txt", FileAccess::WRITE, &err);
		if (f.is_valid()) {
			f->store_string(vformat("pid=%d", (int)::getpid()));
			f->close();
			out["canary_user_dir_write"] = make_canary("allowed");
		} else {
			out["canary_user_dir_write"] = make_canary("blocked", (int)err);
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
			out["canary_sibling_gate_write"] = make_canary("allowed");
		} else {
			out["canary_sibling_gate_write"] = make_canary("blocked", (int)err);
		}
	}

	// Network canaries: per-destination connect attempts. The Seatbelt
	// addend denies the BSD socket-creating syscalls outright, so this
	// connect should never get past socket(); all three destinations
	// report "blocked" by EPERM. See seatbelt_profile.mm.
	{
		auto try_connect = [](uint32_t addr_be, uint16_t port_be) -> Dictionary {
			Dictionary r;
			const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
			if (sock < 0) {
				r["status"] = "blocked";
				r["error"] = (int)errno;
				return r;
			}
			int flags = ::fcntl(sock, F_GETFL, 0);
			if (flags >= 0) {
				::fcntl(sock, F_SETFL, flags | O_NONBLOCK);
			}
			sockaddr_in addr = {};
			addr.sin_family = AF_INET;
			addr.sin_port = port_be;
			addr.sin_addr.s_addr = addr_be;
			const int rc = ::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
			if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
				r["status"] = "allowed";
			} else {
				r["status"] = "blocked";
				r["error"] = (int)errno;
			}
			::close(sock);
			return r;
		};

		// With Seatbelt's (deny network-outbound (remote ip)) all three fail
		// at socket creation. CIDR-level distinction is verified by the
		// broker test (run-sandbox-test.py).
		out["canary_raw_socket_denied"] = try_connect(htonl(0x01010101u), htons(443));
		out["canary_private_ip_blocked"] = try_connect(htonl(0xC0A80101u), htons(80));
		out["canary_localhost_blocked"] = try_connect(htonl(0x7F000001u), htons(22));
		out["canary_public_ip_allowed"] = try_connect(htonl(0x01010101u), htons(443));
	}

	// .pck read canary: load() re-opens the pack on every resource fetch.
	{
		out["canary_pck_read_path"] = p_pack_path;
		if (!p_pack_path.is_empty()) {
			Error err = OK;
			Ref<FileAccess> f = FileAccess::open(p_pack_path, FileAccess::READ, &err);
			if (f.is_valid()) {
				out["canary_pck_read"] = make_canary("allowed");
			} else {
				out["canary_pck_read"] = make_canary("blocked", (int)err);
			}
		} else {
			out["canary_pck_read"] = make_canary("skipped_no_pck_path");
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

	const int seccomp_mode = ::prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
	diag["seccomp_mode"] = seccomp_mode;
	diag["seccomp"] = seccomp_mode_label(seccomp_mode);
	// glibc's prctl is varargs; calling with fewer than 5 args leaves arg2-5
	// as register garbage and the kernel returns -1 EINVAL even for the
	// no-arg PR_GET_* options.
	diag["no_new_privs"] = ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1;
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
