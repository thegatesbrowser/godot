/**************************************************************************/
/*  broker_protocol.h                                                     */
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

#pragma once

#include "core/io/ip_address.h"
#include "core/string/ustring.h"
#include "core/templates/list.h"
#include "core/variant/variant.h"

#include <stdint.h>

// Single source of truth for the wire protocol between the launcher-side
// NetworkBroker and the renderer-side RendererNetClient. Both Request and
// Response are length-prefixed framed messages on the inherited AF_UNIX
// control channel (POSIX) or named pipe (Windows). FDs ride alongside on
// SCM_RIGHTS / WSADuplicateSocket; framing is handled by TGFDPassing.

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

// Renderer -> broker. Wire layout after the framing length prefix:
//   [opcode:1][ip6:16][port:2 LE][hostname_len:1][hostname:N]
struct Request {
	static constexpr int HEADER_SIZE = 20;
	static constexpr int MAX_HOSTNAME = 255;
	static constexpr int MAX_SIZE = HEADER_SIZE + MAX_HOSTNAME;

	uint8_t opcode = 0;
	uint8_t ipv6[16] = {};
	uint16_t port = 0;
	String hostname;

	int serialize(uint8_t *p_buf) const;
	bool parse(const uint8_t *p_buf, int p_len);

	IPAddress get_ip() const;
	void set_ip(const IPAddress &p_ip);
};

// Broker -> renderer. Wire layout after the framing length prefix:
//   [status:1][errno_lo:1][errno_hi:1][payload...]
//
// Payload depends on opcode + status:
//   OP_OPEN_TCP/UDP, ST_OK         : no payload (FD rides on SCM_RIGHTS)
//   OP_RESOLVE_HOSTNAME, ST_OK     : [count:1][ip0:16]..[ip(count-1):16]
//   any error status               : no payload; errno carries OS-level info
//
// The `errno_*` slots are reserved on every response so the renderer can
// always read 3 bytes and decide. For status != ST_SOCKET_ERROR the errno
// slot is zero.
struct Response {
	static constexpr int HEADER_SIZE = 3;
	static constexpr int MAX_RESOLVE_IPS = 8;
	static constexpr int RESOLVE_PAYLOAD_SIZE = 1 + 16 * MAX_RESOLVE_IPS;
	static constexpr int MAX_SIZE = HEADER_SIZE + RESOLVE_PAYLOAD_SIZE;

	uint8_t status = ST_OK;
	uint16_t errno_value = 0;
	int payload_len = 0;
	uint8_t payload[RESOLVE_PAYLOAD_SIZE] = {};

	static Response make_status(Status p_status, int p_errno = 0);

	int serialize(uint8_t *p_buf) const;
	bool parse(const uint8_t *p_buf, int p_len);

	// Helper for OP_RESOLVE_HOSTNAME: pack up to MAX_RESOLVE_IPS IPv6 addresses
	// into the payload. Returns the number of IPs actually stored.
	int set_resolved_ips(const PackedStringArray &p_ips);

	// Inverse of set_resolved_ips, used by the renderer.
	void get_resolved_ips(List<IPAddress> &r_addresses) const;
};

} // namespace BrokerProtocol
