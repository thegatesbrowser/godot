/**************************************************************************/
/*  brokered_net_socket.cpp                                               */
/**************************************************************************/

#include "brokered_net_socket.h"

#include "broker_protocol.h"
#include "cidr_policy.h"
#include "renderer_net_client.h"

#ifdef WINDOWS_ENABLED
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define TG_CLOSE_SOCK(s) ::closesocket((SOCKET)(s))
#define TG_INVALID_SOCK (int)INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#define TG_CLOSE_SOCK(s) ::close(s)
#define TG_INVALID_SOCK (-1)
#endif

namespace {

// Translates the platform's last-error-on-this-fd into a Godot Error code.
// Critical: EAGAIN/EWOULDBLOCK must map to ERR_BUSY, not FAILED — upstream
// callers (StreamPeerTCP::read, mbedtls BIO, ENet's main loop) treat a
// non-BUSY error from a non-blocking socket as a hard disconnect.
Error socket_op_error() {
#ifdef WINDOWS_ENABLED
	const int e = ::WSAGetLastError();
	if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS) {
		return ERR_BUSY;
	}
#else
	const int e = errno;
	if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) {
		return ERR_BUSY;
	}
#endif
	return FAILED;
}

} // namespace

void BrokeredNetSocket::make_default() {
	NetSocket::_create = &BrokeredNetSocket::_create_func;
}

NetSocket *BrokeredNetSocket::_create_func() {
	return memnew(BrokeredNetSocket);
}

Error BrokeredNetSocket::_ensure_connected(const IPAddress &p_ip, uint16_t p_port) {
	if (_connected && _connected_ip == p_ip && _connected_port == p_port) {
		return OK;
	}
	if (_connected) {
		// Different destination on an already-connected socket. The threat
		// model relies on kernel-enforced per-socket destination, so we
		// refuse instead of opening a new one silently.
		return ERR_UNAUTHORIZED;
	}
	const BrokerProtocol::Opcode op = (_requested_type == TYPE_UDP)
			? BrokerProtocol::OP_OPEN_UDP
			: BrokerProtocol::OP_OPEN_TCP;
	int new_fd = -1;
	const Error r = RendererNetClient::open_socket(op, p_ip, p_port, String(), &new_fd);
	if (r != OK) {
		return r;
	}
	if (_sock != TG_INVALID_SOCK) {
		TG_CLOSE_SOCK(_sock);
	}
	_sock = new_fd;
	_connected_ip = p_ip;
	_connected_port = p_port;
	_connected = true;
	_apply_pending_options();
	return OK;
}

void BrokeredNetSocket::_apply_pending_options() {
	if (_sock == TG_INVALID_SOCK) {
		return;
	}
	if (_pending.has_broadcast) {
		set_broadcasting_enabled(_pending.broadcast);
	}
	if (_pending.has_blocking) {
		set_blocking_enabled(_pending.blocking);
	}
	if (_pending.has_ipv6_only) {
		set_ipv6_only_enabled(_pending.ipv6_only);
	}
	if (_pending.has_tcp_no_delay) {
		set_tcp_no_delay_enabled(_pending.tcp_no_delay);
	}
	if (_pending.has_reuse_address) {
		set_reuse_address_enabled(_pending.reuse_address);
	}
	_pending = PendingOptions{};
}

Error BrokeredNetSocket::open(Type p_sock_type, IP::Type &ip_type) {
	_requested_type = p_sock_type;
	_ip_type = (ip_type == IP::TYPE_ANY) ? IP::TYPE_IPV6 : ip_type;
	ip_type = _ip_type;
	// No FD is acquired here. The broker hands one back on the first
	// connect_to_host / sendto so the kernel can enforce destination.
	return OK;
}

void BrokeredNetSocket::close() {
	if (_sock != TG_INVALID_SOCK) {
		TG_CLOSE_SOCK(_sock);
		_sock = TG_INVALID_SOCK;
	}
	_connected = false;
	_connected_port = 0;
	_connected_ip = IPAddress();
}

Error BrokeredNetSocket::connect_to_host(IPAddress p_host, uint16_t p_port) {
	return _ensure_connected(p_host, p_port);
}

Error BrokeredNetSocket::bind(IPAddress /*p_addr*/, uint16_t /*p_port*/) {
	// Server-mode bind is denied: a sandboxed renderer cannot host network
	// services. Gates that need a server run it outside the browser tab.
	return ERR_UNAUTHORIZED;
}

Error BrokeredNetSocket::listen(int /*p_max_pending*/) {
	return ERR_UNAUTHORIZED;
}

Error BrokeredNetSocket::poll(PollType p_type, int timeout) const {
	if (_sock == TG_INVALID_SOCK) {
		return FAILED;
	}

	fd_set rfds;
	fd_set wfds;
	fd_set efds;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
#ifdef WINDOWS_ENABLED
	const SOCKET fd = (SOCKET)_sock;
#else
	const int fd = _sock;
#endif
	FD_SET(fd, &efds);
	if (p_type == POLL_TYPE_IN || p_type == POLL_TYPE_IN_OUT) {
		FD_SET(fd, &rfds);
	}
	if (p_type == POLL_TYPE_OUT || p_type == POLL_TYPE_IN_OUT) {
		FD_SET(fd, &wfds);
	}

	struct timeval tv = {};
	struct timeval *tvp = nullptr;
	if (timeout >= 0) {
		tv.tv_sec = timeout / 1000;
		tv.tv_usec = (timeout % 1000) * 1000;
		tvp = &tv;
	}

#ifdef WINDOWS_ENABLED
	const int rc = ::select(0, &rfds, &wfds, &efds, tvp);
	if (rc == SOCKET_ERROR) {
		return FAILED;
	}
#else
	const int rc = ::select(_sock + 1, &rfds, &wfds, &efds, tvp);
	if (rc < 0) {
		return FAILED;
	}
#endif
	if (rc == 0) {
		return ERR_BUSY;
	}
	return OK;
}

Error BrokeredNetSocket::recv(uint8_t *p_buffer, int p_len, int &r_read) {
	if (_sock == TG_INVALID_SOCK) {
		return FAILED;
	}
#ifdef WINDOWS_ENABLED
	const int got = ::recv((SOCKET)_sock, (char *)p_buffer, p_len, 0);
#else
	const ssize_t got = ::recv(_sock, p_buffer, (size_t)p_len, 0);
#endif
	if (got < 0) {
		return socket_op_error();
	}
	r_read = (int)got;
	return OK;
}

Error BrokeredNetSocket::recvfrom(uint8_t *p_buffer, int p_len, int &r_read, IPAddress &r_ip, uint16_t &r_port, bool p_peek) {
	if (_sock == TG_INVALID_SOCK) {
		return FAILED;
	}
	struct sockaddr_in6 from = {};
	socklen_t from_len = sizeof(from);
#ifdef WINDOWS_ENABLED
	const int got = ::recvfrom((SOCKET)_sock, (char *)p_buffer, p_len,
			p_peek ? MSG_PEEK : 0, (struct sockaddr *)&from, &from_len);
#else
	const ssize_t got = ::recvfrom(_sock, p_buffer, (size_t)p_len,
			p_peek ? MSG_PEEK : 0, (struct sockaddr *)&from, &from_len);
#endif
	if (got < 0) {
		return socket_op_error();
	}
	r_read = (int)got;
	if (_connected) {
		// macOS leaves the from-sockaddr blank on connected UDP; the kernel's
		// 5-tuple filter already guarantees the source equals the cached peer.
		r_ip = _connected_ip;
		r_port = _connected_port;
	} else {
		uint8_t ipv6[16] = {};
		memcpy(ipv6, &from.sin6_addr, 16);
		r_ip.set_ipv6(ipv6);
		r_port = ntohs(from.sin6_port);
	}
	return OK;
}

Error BrokeredNetSocket::send(const uint8_t *p_buffer, int p_len, int &r_sent) {
	if (_sock == TG_INVALID_SOCK) {
		return FAILED;
	}
#ifdef WINDOWS_ENABLED
	const int sent = ::send((SOCKET)_sock, (const char *)p_buffer, p_len, 0);
#else
	const ssize_t sent = ::send(_sock, p_buffer, (size_t)p_len, 0);
#endif
	if (sent < 0) {
		return socket_op_error();
	}
	r_sent = (int)sent;
	return OK;
}

Error BrokeredNetSocket::sendto(const uint8_t *p_buffer, int p_len, int &r_sent, IPAddress p_ip, uint16_t p_port) {
	// Defense-in-depth: the broker already checks this destination at
	// connect() time and the kernel's per-socket peer filter enforces it
	// forever after, but a CIDR check here catches any future path that
	// hands a socket to gate code without going through the broker (or a
	// macOS GDExtension re-connecting an existing FD via libc).
	if (!CIDRPolicy::is_allowed(p_ip)) {
		return ERR_UNAUTHORIZED;
	}
	const Error e = _ensure_connected(p_ip, p_port);
	if (e != OK) {
		return e;
	}
	// Kernel socket is now connected to p_ip:p_port; use plain send().
#ifdef WINDOWS_ENABLED
	const int sent = ::send((SOCKET)_sock, (const char *)p_buffer, p_len, 0);
#else
	const ssize_t sent = ::send(_sock, p_buffer, (size_t)p_len, 0);
#endif
	if (sent < 0) {
		return socket_op_error();
	}
	r_sent = (int)sent;
	return OK;
}

Ref<NetSocket> BrokeredNetSocket::accept(IPAddress & /*r_ip*/, uint16_t & /*r_port*/) {
	// accept() implies a listening socket, which bind/listen already denied.
	return Ref<NetSocket>();
}

bool BrokeredNetSocket::is_open() const {
	return _sock != TG_INVALID_SOCK;
}

int BrokeredNetSocket::get_available_bytes() const {
	if (_sock == TG_INVALID_SOCK) {
		return -1;
	}
#ifdef WINDOWS_ENABLED
	u_long n = 0;
	if (::ioctlsocket((SOCKET)_sock, FIONREAD, &n) != 0) {
		return -1;
	}
	return (int)n;
#else
	int n = 0;
	if (::ioctl(_sock, FIONREAD, &n) != 0) {
		return -1;
	}
	return n;
#endif
}

Error BrokeredNetSocket::get_socket_address(IPAddress *r_ip, uint16_t *r_port) const {
	if (_sock == TG_INVALID_SOCK) {
		return FAILED;
	}
	struct sockaddr_in6 addr = {};
	socklen_t alen = sizeof(addr);
	if (::getsockname(_sock, (struct sockaddr *)&addr, &alen) != 0) {
		return FAILED;
	}
	uint8_t ipv6[16] = {};
	memcpy(ipv6, &addr.sin6_addr, 16);
	r_ip->set_ipv6(ipv6);
	*r_port = ntohs(addr.sin6_port);
	return OK;
}

Error BrokeredNetSocket::set_broadcasting_enabled(bool p_enabled) {
	if (_sock == TG_INVALID_SOCK) {
		_pending.has_broadcast = true;
		_pending.broadcast = p_enabled;
		return OK;
	}
	const int val = p_enabled ? 1 : 0;
#ifdef WINDOWS_ENABLED
	return (::setsockopt((SOCKET)_sock, SOL_SOCKET, SO_BROADCAST,
			(const char *)&val, sizeof(val)) == 0) ? OK : FAILED;
#else
	return (::setsockopt(_sock, SOL_SOCKET, SO_BROADCAST, &val, sizeof(val)) == 0) ? OK : FAILED;
#endif
}

void BrokeredNetSocket::set_blocking_enabled(bool p_enabled) {
	if (_sock == TG_INVALID_SOCK) {
		_pending.has_blocking = true;
		_pending.blocking = p_enabled;
		return;
	}
#ifdef WINDOWS_ENABLED
	u_long nb = p_enabled ? 0 : 1;
	::ioctlsocket((SOCKET)_sock, FIONBIO, &nb);
#else
	int flags = ::fcntl(_sock, F_GETFL, 0);
	if (p_enabled) {
		flags &= ~O_NONBLOCK;
	} else {
		flags |= O_NONBLOCK;
	}
	::fcntl(_sock, F_SETFL, flags);
#endif
}

void BrokeredNetSocket::set_ipv6_only_enabled(bool p_enabled) {
	if (_sock == TG_INVALID_SOCK) {
		_pending.has_ipv6_only = true;
		_pending.ipv6_only = p_enabled;
		return;
	}
	const int val = p_enabled ? 1 : 0;
#ifdef WINDOWS_ENABLED
	::setsockopt((SOCKET)_sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&val, sizeof(val));
#else
	::setsockopt(_sock, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
#endif
}

void BrokeredNetSocket::set_tcp_no_delay_enabled(bool p_enabled) {
	if (_sock == TG_INVALID_SOCK) {
		_pending.has_tcp_no_delay = true;
		_pending.tcp_no_delay = p_enabled;
		return;
	}
	const int val = p_enabled ? 1 : 0;
#ifdef WINDOWS_ENABLED
	::setsockopt((SOCKET)_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&val, sizeof(val));
#else
	::setsockopt(_sock, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
#endif
}

void BrokeredNetSocket::set_reuse_address_enabled(bool p_enabled) {
	if (_sock == TG_INVALID_SOCK) {
		_pending.has_reuse_address = true;
		_pending.reuse_address = p_enabled;
		return;
	}
	const int val = p_enabled ? 1 : 0;
#ifdef WINDOWS_ENABLED
	::setsockopt((SOCKET)_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof(val));
#else
	::setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
#endif
}

Error BrokeredNetSocket::join_multicast_group(const IPAddress & /*p_multi_address*/, const String & /*p_if_name*/) {
	return ERR_UNAVAILABLE;
}

Error BrokeredNetSocket::leave_multicast_group(const IPAddress & /*p_multi_address*/, const String & /*p_if_name*/) {
	return ERR_UNAVAILABLE;
}

BrokeredNetSocket::~BrokeredNetSocket() {
	close();
}
