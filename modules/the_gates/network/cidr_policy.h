/**************************************************************************/
/*  cidr_policy.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/io/ip_address.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"

// Policy gate used by the launcher's NetworkBroker. Decides whether the
// renderer is allowed to open a socket to a given destination.
//
// Default policy: deny RFC 1918, loopback, link-local, and IPv4-mapped IPv6
// for those CIDRs. Public IPv4 / IPv6 are allowed. Matches the threat model
// described in notes/Sandboxing/Network Isolation.md.
class CIDRPolicy {
public:
	// Returns true if the renderer is allowed to open a socket to this
	// destination. Test against the resolved IP (post-DNS); hostnames are
	// not part of the policy because the renderer never reaches DNS — the
	// broker performs hostname resolution itself.
	static bool is_allowed(const IPAddress &p_ip);

	// Convenience: a "why is this blocked?" descriptor for logs and the
	// SANDBOX-DIAG block. Returns an empty string if the address is allowed.
	static String describe_block(const IPAddress &p_ip);

	// TG_NETWORK_BROKER_FORCE_FAIL=1 makes is_allowed() always return false
	// regardless of the address. Used by the harness's negative-broker mode.
	static bool force_fail_active();
};
