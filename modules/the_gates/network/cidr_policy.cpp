/**************************************************************************/
/*  cidr_policy.cpp                                                       */
/**************************************************************************/

#include "cidr_policy.h"

#include <stdlib.h>
#include <string.h>

namespace {

struct V4CIDR {
	uint32_t network; // host byte order
	int prefix;
};

struct V6CIDR {
	uint8_t network[16];
	int prefix;
};

// IPv4 ranges the renderer must not reach. Covers the obvious private
// blocks plus CGN (Tailscale mesh space), multicast, and the reserved /
// "this network" ranges that some kernels route to loopback or the local
// interface.
constexpr V4CIDR v4_blocklist[] = {
	{ 0x00000000u, 8 },   // 0.0.0.0/8         "this network" (RFC 1122)
	{ 0x0A000000u, 8 },   // 10.0.0.0/8        RFC 1918 private
	{ 0x64400000u, 10 },  // 100.64.0.0/10     CGN / Tailscale (RFC 6598)
	{ 0xAC100000u, 12 },  // 172.16.0.0/12     RFC 1918 private
	{ 0xC0A80000u, 16 },  // 192.168.0.0/16    RFC 1918 private
	{ 0x7F000000u, 8 },   // 127.0.0.0/8       loopback
	{ 0xA9FE0000u, 16 },  // 169.254.0.0/16    link-local (cloud metadata)
	{ 0xC6120000u, 15 },  // 198.18.0.0/15     IETF benchmarking
	{ 0xE0000000u, 4 },   // 224.0.0.0/4       multicast (SSDP / mDNS / WS-Discovery)
	{ 0xF0000000u, 4 },   // 240.0.0.0/4       IETF reserved + 255.255.255.255 broadcast
};

// IPv6 ranges. ::ffff:0:0/96 catches IPv4-mapped destinations (a v6 socket
// reaching 192.168.x.x via the mapped form). 64:ff9b::/96 is the well-known
// NAT64 prefix — `64:ff9b::192.168.1.1` reaches RFC 1918 through a v6→v4
// translator and would otherwise bypass the v4 blocklist entirely.
constexpr V6CIDR v6_blocklist[] = {
	// ::1/128 — loopback
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 128 },
	// 64:ff9b::/96 — well-known NAT64 prefix (RFC 6052)
	{ { 0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 96 },
	// 100::/64 — discard-only address block (RFC 6666)
	{ { 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 64 },
	// fc00::/7 — ULA private
	{ { 0xFC, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 7 },
	// fe80::/10 — link-local
	{ { 0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 10 },
	// ff00::/8 — IPv6 multicast (includes link-local multicast ff02::/16)
	{ { 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 8 },
	// ::ffff:0:0/96 — IPv4-mapped (catches v6-socket calls to private v4)
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0 }, 96 },
};

bool match_v4(uint32_t addr_host, const V4CIDR &cidr) {
	if (cidr.prefix == 0) {
		return true;
	}
	const uint32_t mask = (cidr.prefix >= 32) ? 0xFFFFFFFFu : ~((1u << (32 - cidr.prefix)) - 1u);
	return (addr_host & mask) == (cidr.network & mask);
}

bool match_v6(const uint8_t *addr, const V6CIDR &cidr) {
	int bits = cidr.prefix;
	int idx = 0;
	while (bits >= 8) {
		if (addr[idx] != cidr.network[idx]) {
			return false;
		}
		idx++;
		bits -= 8;
	}
	if (bits > 0) {
		const uint8_t mask = static_cast<uint8_t>(0xFFu << (8 - bits));
		if ((addr[idx] & mask) != (cidr.network[idx] & mask)) {
			return false;
		}
	}
	return true;
}

} // namespace

bool CIDRPolicy::force_fail_active() {
	const char *v = ::getenv("TG_NETWORK_BROKER_FORCE_FAIL");
	return v != nullptr && v[0] == '1';
}

bool CIDRPolicy::is_allowed(const IPAddress &p_ip) {
	if (force_fail_active()) {
		return false;
	}
	if (!p_ip.is_valid()) {
		return false;
	}

	const uint8_t *bytes = p_ip.get_ipv6();
	// Godot stores everything as 16-byte IPv6; IPv4 addresses are mapped into
	// the ::ffff:0:0/96 prefix. Detect IPv4-mapped and test against v4 list.
	const bool is_v4_mapped = (bytes[10] == 0xFF && bytes[11] == 0xFF) &&
			(bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4] | bytes[5] |
				bytes[6] | bytes[7] | bytes[8] | bytes[9]) == 0;
	if (is_v4_mapped) {
		const uint32_t v4_host = ((uint32_t)bytes[12] << 24) | ((uint32_t)bytes[13] << 16) |
				((uint32_t)bytes[14] << 8) | (uint32_t)bytes[15];
		for (const V4CIDR &cidr : v4_blocklist) {
			if (match_v4(v4_host, cidr)) {
				return false;
			}
		}
		return true;
	}

	for (const V6CIDR &cidr : v6_blocklist) {
		if (match_v6(bytes, cidr)) {
			return false;
		}
	}
	return true;
}

String CIDRPolicy::describe_block(const IPAddress &p_ip) {
	if (force_fail_active()) {
		return "TG_NETWORK_BROKER_FORCE_FAIL=1";
	}
	if (is_allowed(p_ip)) {
		return String();
	}
	const uint8_t *bytes = p_ip.get_ipv6();
	const bool is_v4_mapped = (bytes[10] == 0xFF && bytes[11] == 0xFF);
	if (is_v4_mapped) {
		const uint32_t v4_host = ((uint32_t)bytes[12] << 24) | ((uint32_t)bytes[13] << 16) |
				((uint32_t)bytes[14] << 8) | (uint32_t)bytes[15];
		for (const V4CIDR &cidr : v4_blocklist) {
			if (match_v4(v4_host, cidr)) {
				return vformat("%s/%d (private/loopback/link-local)", String(p_ip), cidr.prefix);
			}
		}
	}
	for (const V6CIDR &cidr : v6_blocklist) {
		if (match_v6(bytes, cidr)) {
			return vformat("%s/%d (private/loopback/link-local)", String(p_ip), cidr.prefix);
		}
	}
	return String("blocked");
}
