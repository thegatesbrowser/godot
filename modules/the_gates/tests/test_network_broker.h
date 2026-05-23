/**************************************************************************/
/*  test_network_broker.h                                                 */
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

#include "modules/the_gates/network/broker_protocol.h"
#include "modules/the_gates/network/cidr_policy.h"

#include "tests/test_macros.h"

namespace TestNetworkBroker {

TEST_CASE("[BrokerProtocol] Request serialize / parse round-trip") {
	BrokerProtocol::Request orig;
	orig.opcode = BrokerProtocol::OP_OPEN_TCP;
	orig.port = 443;
	orig.hostname = "thegates.io";
	const IPAddress ip("1.2.3.4");
	orig.set_ip(ip);

	uint8_t buf[BrokerProtocol::Request::MAX_SIZE] = {};
	const int len = orig.serialize(buf);
	CHECK(len > BrokerProtocol::Request::HEADER_SIZE);

	BrokerProtocol::Request copy;
	CHECK(copy.parse(buf, len));
	CHECK(copy.opcode == BrokerProtocol::OP_OPEN_TCP);
	CHECK(copy.port == 443);
	CHECK(copy.hostname == "thegates.io");
	CHECK(copy.get_ip() == ip);
}

TEST_CASE("[BrokerProtocol] Request without hostname is HEADER_SIZE bytes") {
	BrokerProtocol::Request orig;
	orig.opcode = BrokerProtocol::OP_OPEN_UDP;
	orig.port = 1234;
	orig.set_ip(IPAddress("5.6.7.8"));

	uint8_t buf[BrokerProtocol::Request::MAX_SIZE] = {};
	const int len = orig.serialize(buf);
	CHECK(len == BrokerProtocol::Request::HEADER_SIZE);

	BrokerProtocol::Request copy;
	CHECK(copy.parse(buf, len));
	CHECK(copy.hostname.is_empty());
}

TEST_CASE("[BrokerProtocol] Request::parse rejects truncated buffer") {
	uint8_t buf[BrokerProtocol::Request::HEADER_SIZE - 1] = {};
	BrokerProtocol::Request r;
	CHECK_FALSE(r.parse(buf, sizeof(buf)));
}

TEST_CASE("[BrokerProtocol] Request::parse rejects truncated hostname") {
	uint8_t buf[BrokerProtocol::Request::MAX_SIZE] = {};
	buf[0] = BrokerProtocol::OP_OPEN_TCP;
	// Claim 50 bytes of hostname but only ship 10.
	buf[19] = 50;
	BrokerProtocol::Request r;
	CHECK_FALSE(r.parse(buf, BrokerProtocol::Request::HEADER_SIZE + 10));
}

TEST_CASE("[BrokerProtocol] Response::make_status carries status + errno") {
	const auto resp = BrokerProtocol::Response::make_status(BrokerProtocol::ST_SOCKET_ERROR, 42);
	CHECK(resp.status == BrokerProtocol::ST_SOCKET_ERROR);
	CHECK(resp.errno_value == 42);
	CHECK(resp.payload_len == 0);
}

TEST_CASE("[BrokerProtocol] Response status-only round-trip") {
	const auto orig = BrokerProtocol::Response::make_status(BrokerProtocol::ST_DENIED_POLICY, 0);
	uint8_t buf[BrokerProtocol::Response::MAX_SIZE] = {};
	const int len = orig.serialize(buf);
	CHECK(len == BrokerProtocol::Response::HEADER_SIZE);

	BrokerProtocol::Response copy;
	CHECK(copy.parse(buf, len));
	CHECK(copy.status == BrokerProtocol::ST_DENIED_POLICY);
	CHECK(copy.payload_len == 0);
}

TEST_CASE("[BrokerProtocol] Response set_resolved_ips round-trip") {
	PackedStringArray ips;
	ips.push_back("1.1.1.1");
	ips.push_back("2606:4700:4700::1111");

	BrokerProtocol::Response orig;
	orig.status = BrokerProtocol::ST_OK;
	const int count = orig.set_resolved_ips(ips);
	CHECK(count == 2);
	CHECK(orig.payload_len == 1 + 2 * 16);

	uint8_t buf[BrokerProtocol::Response::MAX_SIZE] = {};
	const int len = orig.serialize(buf);

	BrokerProtocol::Response copy;
	CHECK(copy.parse(buf, len));
	List<IPAddress> resolved;
	copy.get_resolved_ips(resolved);
	CHECK(resolved.size() == 2);
	CHECK(resolved.front()->get() == IPAddress("1.1.1.1"));
}

TEST_CASE("[BrokerProtocol] Response set_resolved_ips caps at MAX_RESOLVE_IPS") {
	PackedStringArray ips;
	for (int i = 0; i < 20; ++i) {
		ips.push_back(vformat("10.0.0.%d", i));
	}
	BrokerProtocol::Response resp;
	const int count = resp.set_resolved_ips(ips);
	CHECK(count == BrokerProtocol::Response::MAX_RESOLVE_IPS);
}

// CIDR policy: rejects every documented private/reserved range and allows
// the public-IP cases the broker would forward to.
TEST_CASE("[CIDRPolicy] private + reserved IPv4 blocked") {
	const char *blocked[] = {
		"0.0.0.0",
		"10.0.0.1",
		"100.64.0.1", // CGN / Tailscale
		"127.0.0.1",
		"169.254.169.254", // cloud metadata
		"172.16.0.1",
		"192.168.1.1",
		"198.18.0.1", // IETF benchmarking
		"224.0.0.251", // mDNS
		"239.255.255.250", // SSDP
		"255.255.255.255",
	};
	for (const char *addr : blocked) {
		const IPAddress ip(addr);
		CHECK_MESSAGE(!CIDRPolicy::is_allowed(ip), String(addr));
	}
}

TEST_CASE("[CIDRPolicy] private + reserved IPv6 blocked") {
	const char *blocked[] = {
		"::1", // loopback
		"fc00::1", // ULA
		"fd00::1", // ULA
		"fe80::1", // link-local
		"ff02::1", // multicast
		"64:ff9b::c0a8:101", // NAT64 -> 192.168.1.1
		"100::1", // discard
	};
	for (const char *addr : blocked) {
		const IPAddress ip(addr);
		CHECK_MESSAGE(!CIDRPolicy::is_allowed(ip), String(addr));
	}

	// IPv4-mapped IPv6 (`::ffff:X.Y.Z.W`): the production path constructs
	// this via IPAddress::set_ipv4 from the kernel's sockaddr_in bytes, so
	// exercise the byte API directly. Godot's string parser for the
	// `::ffff:X.Y.Z.W` notation has an unrelated off-by-two bug we don't
	// rely on.
	IPAddress mapped_private;
	const uint8_t private_v4[4] = { 192, 168, 1, 1 };
	mapped_private.set_ipv4(private_v4);
	CHECK_FALSE(CIDRPolicy::is_allowed(mapped_private));
}

TEST_CASE("[CIDRPolicy] public IPv4 + IPv6 allowed") {
	const char *allowed[] = {
		"1.1.1.1",
		"8.8.8.8",
		"172.217.16.46", // Google
		"2606:4700:4700::1111", // Cloudflare
		"2001:4860:4860::8888", // Google
	};
	for (const char *addr : allowed) {
		const IPAddress ip(addr);
		CHECK_MESSAGE(CIDRPolicy::is_allowed(ip), String(addr));
	}
}

TEST_CASE("[CIDRPolicy] invalid IP rejected") {
	CHECK_FALSE(CIDRPolicy::is_allowed(IPAddress()));
}

} // namespace TestNetworkBroker
