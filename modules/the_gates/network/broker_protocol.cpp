/**************************************************************************/
/*  broker_protocol.cpp                                                   */
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

#include "broker_protocol.h"

#include <string.h>

namespace BrokerProtocol {

int Request::serialize(uint8_t *p_buf) const {
	memset(p_buf, 0, HEADER_SIZE);
	p_buf[0] = opcode;
	memcpy(p_buf + 1, ipv6, 16);
	p_buf[17] = (uint8_t)(port & 0xFF);
	p_buf[18] = (uint8_t)((port >> 8) & 0xFF);
	const CharString cs = hostname.utf8();
	const int name_len = cs.length() > MAX_HOSTNAME ? MAX_HOSTNAME : cs.length();
	p_buf[19] = (uint8_t)name_len;
	if (name_len > 0) {
		memcpy(p_buf + HEADER_SIZE, cs.get_data(), (size_t)name_len);
	}
	return HEADER_SIZE + name_len;
}

bool Request::parse(const uint8_t *p_buf, int p_len) {
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

IPAddress Request::get_ip() const {
	IPAddress ip;
	ip.set_ipv6(ipv6);
	return ip;
}

void Request::set_ip(const IPAddress &p_ip) {
	if (p_ip.is_valid()) {
		memcpy(ipv6, p_ip.get_ipv6(), 16);
	} else {
		memset(ipv6, 0, 16);
	}
}

Response Response::make_status(Status p_status, int p_errno) {
	Response r;
	r.status = (uint8_t)p_status;
	r.errno_value = (uint16_t)(p_errno & 0xFFFF);
	r.payload_len = 0;
	return r;
}

int Response::serialize(uint8_t *p_buf) const {
	p_buf[0] = status;
	p_buf[1] = (uint8_t)(errno_value & 0xFF);
	p_buf[2] = (uint8_t)((errno_value >> 8) & 0xFF);
	if (payload_len > 0) {
		memcpy(p_buf + HEADER_SIZE, payload, (size_t)payload_len);
	}
	return HEADER_SIZE + payload_len;
}

bool Response::parse(const uint8_t *p_buf, int p_len) {
	if (p_len < HEADER_SIZE) {
		return false;
	}
	status = p_buf[0];
	errno_value = (uint16_t)p_buf[1] | ((uint16_t)p_buf[2] << 8);
	payload_len = p_len - HEADER_SIZE;
	if (payload_len > (int)sizeof(payload)) {
		return false;
	}
	if (payload_len > 0) {
		memcpy(payload, p_buf + HEADER_SIZE, (size_t)payload_len);
	}
	return true;
}

int Response::set_resolved_ips(const PackedStringArray &p_ips) {
	int count = 0;
	for (int i = 0; i < p_ips.size() && count < MAX_RESOLVE_IPS; ++i) {
		const IPAddress ip(p_ips[i]);
		if (!ip.is_valid()) {
			continue;
		}
		memcpy(payload + 1 + count * 16, ip.get_ipv6(), 16);
		count++;
	}
	payload[0] = (uint8_t)count;
	payload_len = 1 + count * 16;
	return count;
}

void Response::get_resolved_ips(List<IPAddress> &r_addresses) const {
	if (payload_len < 1) {
		return;
	}
	const int count = payload[0];
	for (int i = 0; i < count && (1 + (i + 1) * 16) <= payload_len; ++i) {
		IPAddress ip;
		ip.set_ipv6(payload + 1 + i * 16);
		if (ip.is_valid()) {
			r_addresses.push_back(ip);
		}
	}
}

} // namespace BrokerProtocol
