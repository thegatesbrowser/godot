/**************************************************************************/
/*  broker_protocol.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/*                                                                        */
/*  Single source of truth for the wire protocol between the launcher-    */
/*  side NetworkBroker and the renderer-side RendererNetClient. Owning    */
/*  both ends of the protocol in one header keeps the opcodes, status     */
/*  codes, and request layout in lockstep — if one drifts, the wire       */
/*  silently breaks.                                                      */
/**************************************************************************/

#pragma once

#include "core/io/ip_address.h"
#include "core/string/ustring.h"

#include <stdint.h>
#include <string.h>

namespace BrokerProtocol {

enum Opcode : uint8_t {
	OP_OPEN_TCP = 0x01,
	OP_OPEN_UDP = 0x02,
	OP_RESOLVE_HOSTNAME = 0x03,
};

enum Status : uint8_t {
	ST_OK = 0,
	ST_DENIED_POLICY = 1,
	ST_DENIED_FORCE_FAIL = 2,
	ST_DNS_FAILED = 3,
	ST_SOCKET_ERROR = 4,
	ST_BAD_REQUEST = 5,
};

// Wire layout of one request, after the framing length prefix:
//   [opcode:1][ip6:16][port:2 little-endian][hostname_len:1][hostname:N]
struct Request {
	static constexpr int MAX_SIZE = 1 + 16 + 2 + 1 + 255;
	static constexpr int HEADER_SIZE = 20;
	static constexpr int MAX_HOSTNAME = 255;

	uint8_t opcode = 0;
	uint8_t ipv6[16] = {};
	uint16_t port = 0;
	String hostname;

	// Serialize into p_buf (must hold MAX_SIZE bytes). Returns total bytes written.
	int serialize(uint8_t *p_buf) const {
		memset(p_buf, 0, HEADER_SIZE);
		p_buf[0] = opcode;
		memcpy(p_buf + 1, ipv6, 16);
		p_buf[17] = (uint8_t)(port & 0xFF);
		p_buf[18] = (uint8_t)((port >> 8) & 0xFF);
		const CharString cs = hostname.utf8();
		const int name_len = (cs.length() > MAX_HOSTNAME) ? MAX_HOSTNAME : cs.length();
		p_buf[19] = (uint8_t)name_len;
		if (name_len > 0) {
			memcpy(p_buf + HEADER_SIZE, cs.get_data(), (size_t)name_len);
		}
		return HEADER_SIZE + name_len;
	}

	// Parse from a received buffer. Returns true on success, false if the
	// buffer is too short or the hostname length overflows.
	bool parse(const uint8_t *p_buf, int p_len) {
		if (p_len < HEADER_SIZE) {
			return false;
		}
		opcode = p_buf[0];
		memcpy(ipv6, p_buf + 1, 16);
		port = (uint16_t)p_buf[17] | ((uint16_t)p_buf[18] << 8);
		const uint8_t name_len = p_buf[19];
		if (p_len < HEADER_SIZE + name_len) {
			return false;
		}
		if (name_len > 0) {
			hostname = String::utf8((const char *)(p_buf + HEADER_SIZE), (int)name_len);
		} else {
			hostname = String();
		}
		return true;
	}

	IPAddress get_ip() const {
		IPAddress ip;
		ip.set_ipv6(ipv6);
		return ip;
	}

	void set_ip(const IPAddress &p_ip) {
		if (p_ip.is_valid()) {
			memcpy(ipv6, p_ip.get_ipv6(), 16);
		} else {
			memset(ipv6, 0, 16);
		}
	}
};

} // namespace BrokerProtocol
