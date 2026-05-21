/**************************************************************************/
/*  network_broker.cpp                                                    */
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
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using BrokerProtocol::Request;
using BrokerProtocol::Status;

#ifdef WINDOWS_ENABLED
constexpr int TG_INVALID_NATIVE_SOCK = (int)INVALID_SOCKET;
#else
constexpr int TG_INVALID_NATIVE_SOCK = -1;
#endif

int create_native_socket(int family, bool stream) {
#ifdef WINDOWS_ENABLED
	return (int)::WSASocketW(family, stream ? SOCK_STREAM : SOCK_DGRAM,
			stream ? IPPROTO_TCP : IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
#else
	return ::socket(family, stream ? SOCK_STREAM : SOCK_DGRAM,
			stream ? IPPROTO_TCP : IPPROTO_UDP);
#endif
}

void close_native_socket(int fd) {
	if (fd < 0) {
		return;
	}
#ifdef WINDOWS_ENABLED
	::closesocket((SOCKET)fd);
#else
	::close(fd);
#endif
}

int last_socket_error() {
#ifdef WINDOWS_ENABLED
	return (int)::WSAGetLastError();
#else
	return errno;
#endif
}

void send_status(int p_control_fd, void *p_target_handle, Status p_status, int p_errno = 0) {
	uint8_t resp[3];
	resp[0] = (uint8_t)p_status;
	resp[1] = (uint8_t)(p_errno & 0xFF);
	resp[2] = (uint8_t)((p_errno >> 8) & 0xFF);
	TGFDPassing::send_msg(p_control_fd, -1, p_target_handle, resp, sizeof(resp));
}

bool connect_is_fatal(int p_err) {
#ifdef WINDOWS_ENABLED
	return p_err != WSAEWOULDBLOCK && p_err != WSAEINPROGRESS;
#else
	return p_err != EINPROGRESS;
#endif
}

} // namespace

Error NetworkBroker::start(int p_peer_fd) {
	if (p_peer_fd < 0) {
		return ERR_INVALID_PARAMETER;
	}
	peer_fd = p_peer_fd;
	shutdown_requested.store(false);
	service_thread.start(&NetworkBroker::_service_thread_func, this);
	return OK;
}

void NetworkBroker::request_shutdown() {
	shutdown_requested.store(true);
	if (peer_fd >= 0) {
#ifdef WINDOWS_ENABLED
		::CloseHandle((HANDLE)(intptr_t)peer_fd);
#else
		::shutdown(peer_fd, SHUT_RDWR);
		::close(peer_fd);
#endif
		peer_fd = -1;
	}
}

void NetworkBroker::shutdown() {
	request_shutdown();
	if (service_thread.is_started()) {
		service_thread.wait_to_finish();
	}
}

void NetworkBroker::_service_thread_func(void *p_userdata) {
	NetworkBroker *self = static_cast<NetworkBroker *>(p_userdata);
	self->_serve();
}

void NetworkBroker::_serve() {
	using namespace BrokerProtocol;

	while (!shutdown_requested.load()) {
		uint8_t req_buf[Request::MAX_SIZE];
		int req_len = 0;
		int stray_fd = -1;
		const Error r = TGFDPassing::recv_msg(peer_fd, &stray_fd, req_buf, (int)sizeof(req_buf), &req_len);
		if (stray_fd >= 0) {
			close_native_socket(stray_fd);
		}
		if (r != OK) {
			break;
		}

		Request req;
		if (!req.parse(req_buf, req_len)) {
			send_status(peer_fd, target_process_handle, ST_BAD_REQUEST);
			stats.requests_denied.fetch_add(1);
			continue;
		}

		stats.requests_total.fetch_add(1);

		// Pure-DNS query — no socket created. Response:
		//   [ST_OK][count:1][ip0:16]..[ipN:16]  (cap N at 8)
		if (req.opcode == OP_RESOLVE_HOSTNAME) {
			stats.dns_resolutions.fetch_add(1);
			const PackedStringArray ips = IP::get_singleton()->resolve_hostname_addresses(req.hostname);
			if (ips.is_empty()) {
				send_status(peer_fd, target_process_handle, ST_DNS_FAILED);
				stats.requests_denied.fetch_add(1);
				continue;
			}
			uint8_t resp[2 + 16 * 8] = {};
			resp[0] = ST_OK;
			int count = 0;
			for (int i = 0; i < ips.size() && count < 8; ++i) {
				IPAddress ip(ips[i]);
				if (!ip.is_valid()) {
					continue;
				}
				memcpy(resp + 2 + count * 16, ip.get_ipv6(), 16);
				count++;
			}
			resp[1] = (uint8_t)count;
			TGFDPassing::send_msg(peer_fd, -1, target_process_handle, resp, 2 + count * 16);
			stats.requests_allowed.fetch_add(1);
			continue;
		}

		if (req.opcode != OP_OPEN_TCP && req.opcode != OP_OPEN_UDP) {
			send_status(peer_fd, target_process_handle, ST_BAD_REQUEST);
			stats.requests_denied.fetch_add(1);
			continue;
		}

		// Resolve hostname if present; otherwise use the IP bytes from request.
		IPAddress dest;
		if (!req.hostname.is_empty()) {
			stats.dns_resolutions.fetch_add(1);
			dest = IP::get_singleton()->resolve_hostname(req.hostname);
			if (!dest.is_valid()) {
				send_status(peer_fd, target_process_handle, ST_DNS_FAILED);
				stats.requests_denied.fetch_add(1);
				continue;
			}
		} else {
			dest = req.get_ip();
		}

		if (!CIDRPolicy::is_allowed(dest)) {
			const String why = CIDRPolicy::describe_block(dest);
			print_line(vformat("[NETWORK-BROKER] DENY %s: %s", String(dest), why));
			send_status(peer_fd, target_process_handle,
					CIDRPolicy::force_fail_active() ? ST_DENIED_FORCE_FAIL : ST_DENIED_POLICY);
			stats.requests_denied.fetch_add(1);
			continue;
		}

		// AF_INET6 dual-stack handles both v4 (via IPv4-mapped) and v6.
		const bool is_stream = (req.opcode == OP_OPEN_TCP);
		const int sock = create_native_socket(AF_INET6, is_stream);
		if (sock == TG_INVALID_NATIVE_SOCK) {
			send_status(peer_fd, target_process_handle, ST_SOCKET_ERROR, last_socket_error());
			stats.requests_denied.fetch_add(1);
			continue;
		}
		// Force dual-stack regardless of the system's net.inet6.ip6.v6only
		// sysctl — we connect to IPv4-mapped addresses for v4 destinations.
		const int v6only = 0;
#ifdef WINDOWS_ENABLED
		::setsockopt((SOCKET)sock, IPPROTO_IPV6, IPV6_V6ONLY,
				(const char *)&v6only, sizeof(v6only));
#else
		::setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif

		struct sockaddr_in6 addr = {};
		addr.sin6_family = AF_INET6;
		addr.sin6_port = htons(req.port);
		memcpy(&addr.sin6_addr, dest.get_ipv6(), 16);
		if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
			const int e = last_socket_error();
			if (connect_is_fatal(e)) {
				close_native_socket(sock);
				send_status(peer_fd, target_process_handle, ST_SOCKET_ERROR, e);
				stats.requests_denied.fetch_add(1);
				continue;
			}
		}

		uint8_t resp[3] = { ST_OK, 0, 0 };
		const Error sent = TGFDPassing::send_msg(peer_fd, sock, target_process_handle, resp, sizeof(resp));
		close_native_socket(sock);
		if (sent != OK) {
			break;
		}
		stats.requests_allowed.fetch_add(1);
		print_line(vformat("[NETWORK-BROKER] ALLOW %s %s:%d",
				String::utf8(is_stream ? "tcp" : "udp"), String(dest), (int)req.port));
	}
}

Dictionary NetworkBroker::state() const {
	Dictionary out;
	out["requests_total"] = (int64_t)stats.requests_total.load();
	out["requests_allowed"] = (int64_t)stats.requests_allowed.load();
	out["requests_denied"] = (int64_t)stats.requests_denied.load();
	out["dns_resolutions"] = (int64_t)stats.dns_resolutions.load();
	out["status"] = (peer_fd >= 0) ? "running" : "stopped";
	return out;
}

NetworkBroker::~NetworkBroker() {
	shutdown();
}
