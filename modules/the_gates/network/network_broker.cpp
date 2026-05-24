/**************************************************************************/
/*  network_broker.cpp                                                    */
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

#include "network_broker.h"

#include "broker_protocol.h"
#include "cidr_policy.h"
#include "fd_passing.h"

#include "core/io/ip.h"
#include "core/string/print_string.h"

#ifdef WINDOWS_ENABLED
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using BrokerProtocol::Opcode;
using BrokerProtocol::Request;
using BrokerProtocol::Response;
using BrokerProtocol::Status;

#ifdef WINDOWS_ENABLED
constexpr int TG_INVALID_NATIVE_SOCK = (int)INVALID_SOCKET;
#else
constexpr int TG_INVALID_NATIVE_SOCK = -1;
#endif

int create_native_socket(int p_family, bool p_stream) {
#ifdef WINDOWS_ENABLED
	return (int)::WSASocketW(p_family, p_stream ? SOCK_STREAM : SOCK_DGRAM,
			p_stream ? IPPROTO_TCP : IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
#else
	return ::socket(p_family, p_stream ? SOCK_STREAM : SOCK_DGRAM,
			p_stream ? IPPROTO_TCP : IPPROTO_UDP);
#endif
}

void close_native_socket(int p_fd) {
	if (p_fd < 0) {
		return;
	}
#ifdef WINDOWS_ENABLED
	::closesocket((SOCKET)p_fd);
#else
	::close(p_fd);
#endif
}

int last_socket_error() {
#ifdef WINDOWS_ENABLED
	return (int)::WSAGetLastError();
#else
	return errno;
#endif
}

bool connect_is_fatal(int p_err) {
#ifdef WINDOWS_ENABLED
	return p_err != WSAEWOULDBLOCK && p_err != WSAEINPROGRESS;
#else
	return p_err != EINPROGRESS;
#endif
}

void set_socket_non_blocking(int p_fd) {
#ifdef WINDOWS_ENABLED
	u_long non_blocking = 1;
	::ioctlsocket((SOCKET)p_fd, FIONBIO, &non_blocking);
#else
	const int flags = ::fcntl(p_fd, F_GETFL, 0);
	::fcntl(p_fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

void disable_ipv6_only(int p_fd) {
	const int v6only = 0;
#ifdef WINDOWS_ENABLED
	::setsockopt((SOCKET)p_fd, IPPROTO_IPV6, IPV6_V6ONLY,
			(const char *)&v6only, sizeof(v6only));
#else
	::setsockopt(p_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
}

// Resolve a Request's destination. If hostname is set, the broker resolves
// via getaddrinfo (which is allowed in the launcher process). Otherwise the
// request carries pre-resolved IP bytes from the renderer (rare).
IPAddress resolve_destination(const Request &p_req, bool *r_dns_used) {
	*r_dns_used = !p_req.hostname.is_empty();
	if (*r_dns_used) {
		return IP::get_singleton()->resolve_hostname(p_req.hostname);
	}
	return p_req.get_ip();
}

} // namespace

void NetworkBroker::set_target_process_handle(void *p_handle) {
#ifdef WINDOWS_ENABLED
	if (target_process_handle != nullptr) {
		::CloseHandle((HANDLE)target_process_handle);
		target_process_handle = nullptr;
	}
	if (p_handle == nullptr) {
		return;
	}
	HANDLE dup = nullptr;
	const HANDLE self = ::GetCurrentProcess();
	if (!::DuplicateHandle(self, (HANDLE)p_handle, self, &dup,
				PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
				FALSE, 0)) {
		ERR_PRINT(vformat("NetworkBroker: DuplicateHandle(target) failed (win=%d)",
				(int)::GetLastError()));
		return;
	}
	target_process_handle = dup;
#else
	target_process_handle = p_handle;
#endif
}

Error NetworkBroker::start(intptr_t p_peer_handle) {
	if (p_peer_handle < 0) {
		return ERR_INVALID_PARAMETER;
	}
	peer_handle = p_peer_handle;
	shutdown_requested.store(false);
	service_thread.start(&NetworkBroker::_service_thread_func, this);
	return OK;
}

void NetworkBroker::request_shutdown() {
	shutdown_requested.store(true);
	if (peer_handle >= 0) {
#ifdef WINDOWS_ENABLED
		::CloseHandle((HANDLE)peer_handle);
#else
		const int fd = (int)peer_handle;
		::shutdown(fd, SHUT_RDWR);
		::close(fd);
#endif
		peer_handle = -1;
	}
}

void NetworkBroker::shutdown() {
	request_shutdown();
	if (service_thread.is_started()) {
		service_thread.wait_to_finish();
	}
}

void NetworkBroker::_service_thread_func(void *p_userdata) {
	static_cast<NetworkBroker *>(p_userdata)->_serve();
}

void NetworkBroker::_serve() {
	using namespace BrokerProtocol;

	while (!shutdown_requested.load()) {
		Request req;
		if (!_recv_request(req)) {
			break;
		}
		stats.requests_total.fetch_add(1);

		int payload_fd = -1;
		Response resp;
		switch ((Opcode)req.opcode) {
			case OP_RESOLVE_HOSTNAME:
				resp = _handle_resolve(req);
				break;
			case OP_OPEN_TCP:
			case OP_OPEN_UDP:
				resp = _handle_open_socket(req, &payload_fd);
				break;
			default:
				resp = Response::make_status(ST_BAD_REQUEST);
				break;
		}

		if (resp.status == ST_OK) {
			stats.requests_allowed.fetch_add(1);
		} else {
			stats.requests_denied.fetch_add(1);
		}

		const Error sent = _send_response(resp, payload_fd);
		if (payload_fd >= 0) {
			close_native_socket(payload_fd);
		}
		if (sent != OK) {
			break;
		}
	}
}

bool NetworkBroker::_recv_request(BrokerProtocol::Request &r_req) {
	uint8_t buf[Request::MAX_SIZE];
	int len = 0;
	int stray_fd = -1;
	const Error r = TGFDPassing::recv_msg(peer_handle, &stray_fd, buf, (int)sizeof(buf), &len);
	if (stray_fd >= 0) {
		close_native_socket(stray_fd);
	}
	if (r != OK) {
		return false;
	}
	if (!r_req.parse(buf, len)) {
		// Reply with ST_BAD_REQUEST and continue serving. Returning false here
		// would tear down the channel for a single malformed request, which
		// punishes the gate code for a recoverable error.
		Response bad = Response::make_status(BrokerProtocol::ST_BAD_REQUEST);
		_send_response(bad, -1);
		stats.requests_denied.fetch_add(1);
		return _recv_request(r_req);
	}
	return true;
}

Error NetworkBroker::_send_response(const BrokerProtocol::Response &p_resp, int p_fd) {
	uint8_t buf[Response::MAX_SIZE];
	const int len = p_resp.serialize(buf);
	return TGFDPassing::send_msg(peer_handle, p_fd, target_process_handle, buf, len);
}

BrokerProtocol::Response NetworkBroker::_handle_resolve(const BrokerProtocol::Request &p_req) {
	stats.dns_resolutions.fetch_add(1);
	const PackedStringArray ips = IP::get_singleton()->resolve_hostname_addresses(p_req.hostname);
	if (ips.is_empty()) {
		return Response::make_status(BrokerProtocol::ST_DNS_FAILED);
	}
	Response resp;
	resp.status = BrokerProtocol::ST_OK;
	resp.set_resolved_ips(ips);
	return resp;
}

BrokerProtocol::Response NetworkBroker::_handle_open_socket(const BrokerProtocol::Request &p_req, int *r_fd) {
	*r_fd = -1;

	bool dns_used = false;
	const IPAddress dest = resolve_destination(p_req, &dns_used);
	if (dns_used) {
		stats.dns_resolutions.fetch_add(1);
		if (!dest.is_valid()) {
			return Response::make_status(BrokerProtocol::ST_DNS_FAILED);
		}
	}

	if (!CIDRPolicy::is_allowed(dest)) {
		const String why = CIDRPolicy::describe_block(dest);
		print_line(vformat("[NETWORK-BROKER] DENY %s: %s", String(dest), why));
		return Response::make_status(CIDRPolicy::force_fail_active()
						? BrokerProtocol::ST_DENIED_FORCE_FAIL
						: BrokerProtocol::ST_DENIED_POLICY);
	}

	const bool is_stream = (p_req.opcode == BrokerProtocol::OP_OPEN_TCP);
	const int sock = create_native_socket(AF_INET6, is_stream);
	if (sock == TG_INVALID_NATIVE_SOCK) {
		return Response::make_status(BrokerProtocol::ST_SOCKET_ERROR, last_socket_error());
	}
	disable_ipv6_only(sock);
	set_socket_non_blocking(sock);

	struct sockaddr_in6 addr = {};
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(p_req.port);
	memcpy(&addr.sin6_addr, dest.get_ipv6(), 16);
	// Non-blocking connect: kernel binds the destination at connect() time
	// regardless of blocking mode, so CIDR enforcement holds. The renderer's
	// BrokeredNetSocket::poll detects completion via SO_ERROR.
	if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		const int e = last_socket_error();
		if (connect_is_fatal(e)) {
			close_native_socket(sock);
			return Response::make_status(BrokerProtocol::ST_SOCKET_ERROR, e);
		}
	}

	print_line(vformat("[NETWORK-BROKER] ALLOW %s %s:%d",
			String::utf8(is_stream ? "tcp" : "udp"), String(dest), (int)p_req.port));

	*r_fd = sock;
	return Response::make_status(BrokerProtocol::ST_OK);
}

Dictionary NetworkBroker::state() const {
	Dictionary out;
	out["requests_total"] = (int64_t)stats.requests_total.load();
	out["requests_allowed"] = (int64_t)stats.requests_allowed.load();
	out["requests_denied"] = (int64_t)stats.requests_denied.load();
	out["dns_resolutions"] = (int64_t)stats.dns_resolutions.load();
	out["status"] = (peer_handle >= 0) ? "running" : "stopped";
	return out;
}

NetworkBroker::~NetworkBroker() {
	shutdown();
#ifdef WINDOWS_ENABLED
	if (target_process_handle != nullptr) {
		::CloseHandle((HANDLE)target_process_handle);
		target_process_handle = nullptr;
	}
#endif
}
