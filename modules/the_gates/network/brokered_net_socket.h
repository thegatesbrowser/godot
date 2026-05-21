/**************************************************************************/
/*  brokered_net_socket.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/*                                                                        */
/*  NetSocket subclass installed as the engine factory in renderer builds */
/*  (TG_RENDERER). Every HTTPClient, WebSocketPeer, StreamPeerTCP,        */
/*  PacketPeerUDP, UDPServer, ENet host, and DTLS peer in the renderer    */
/*  goes through this — the OS sandbox forbids raw socket().              */
/*                                                                        */
/*  TCP flow:                                                             */
/*    open(TYPE_TCP)          — records type; no kernel FD yet.           */
/*    connect_to_host(ip, p)  — broker connects a kernel TCP socket and   */
/*                              ships the FD. Kernel enforces destination */
/*                              from here on.                             */
/*                                                                        */
/*  UDP flow (client-only — server bind is denied):                       */
/*    open(TYPE_UDP)          — records type; no kernel FD yet.           */
/*    connect_to_host(ip, p)  — broker connects a kernel UDP socket and   */
/*                              ships the FD; destination is locked.      */
/*    sendto(ip, p, ...)      — first call connects the kernel socket to  */
/*                              `ip:p`; subsequent sendto to a different  */
/*                              destination returns ERR_UNAUTHORIZED.     */
/*                                                                        */
/*  Server-mode bind() (TCP listen / UDP bind to a non-wildcard local     */
/*  port) is denied — renderers cannot host servers from inside a gate.   */
/**************************************************************************/

#pragma once

#include "core/io/net_socket.h"

class BrokeredNetSocket : public NetSocket {
private:
	int _sock = -1;
	Type _requested_type = TYPE_NONE;
	IP::Type _ip_type = IP::TYPE_NONE;

	// Destination the kernel socket is connected to. Used by sendto to
	// detect "different destination than the first one we locked."
	IPAddress _connected_ip;
	uint16_t _connected_port = 0;
	bool _connected = false;

	// Socket options requested between open() and the first FD acquisition.
	// Replayed onto the kernel FD as soon as the broker hands it back.
	struct PendingOptions {
		bool has_broadcast = false;
		bool broadcast = false;
		bool has_blocking = false;
		bool blocking = false;
		bool has_ipv6_only = false;
		bool ipv6_only = false;
		bool has_tcp_no_delay = false;
		bool tcp_no_delay = false;
		bool has_reuse_address = false;
		bool reuse_address = false;
	} _pending;

	Error _ensure_connected(const IPAddress &p_ip, uint16_t p_port);
	void _apply_pending_options();

protected:
	static NetSocket *_create_func();

public:
	// Replaces NetSocket::_create with one that returns BrokeredNetSocket.
	// Idempotent; safe to call multiple times.
	static void make_default();

	// NetSocket interface.
	Error open(Type p_sock_type, IP::Type &ip_type) override;
	void close() override;
	Error bind(IPAddress p_addr, uint16_t p_port) override;
	Error listen(int p_max_pending) override;
	Error connect_to_host(IPAddress p_host, uint16_t p_port) override;
	Error poll(PollType p_type, int timeout) const override;
	Error recv(uint8_t *p_buffer, int p_len, int &r_read) override;
	Error recvfrom(uint8_t *p_buffer, int p_len, int &r_read, IPAddress &r_ip, uint16_t &r_port, bool p_peek = false) override;
	Error send(const uint8_t *p_buffer, int p_len, int &r_sent) override;
	Error sendto(const uint8_t *p_buffer, int p_len, int &r_sent, IPAddress p_ip, uint16_t p_port) override;
	Ref<NetSocket> accept(IPAddress &r_ip, uint16_t &r_port) override;

	bool is_open() const override;
	int get_available_bytes() const override;
	Error get_socket_address(IPAddress *r_ip, uint16_t *r_port) const override;

	Error set_broadcasting_enabled(bool p_enabled) override;
	void set_blocking_enabled(bool p_enabled) override;
	void set_ipv6_only_enabled(bool p_enabled) override;
	void set_tcp_no_delay_enabled(bool p_enabled) override;
	void set_reuse_address_enabled(bool p_enabled) override;
	Error join_multicast_group(const IPAddress &p_multi_address, const String &p_if_name) override;
	Error leave_multicast_group(const IPAddress &p_multi_address, const String &p_if_name) override;

	BrokeredNetSocket() = default;
	~BrokeredNetSocket() override;
};
