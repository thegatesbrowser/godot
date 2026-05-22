/**************************************************************************/
/*  renderer_net_client.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/*                                                                        */
/*  Renderer-side owner of the broker control channel. One instance per   */
/*  renderer process; both BrokeredNetSocket (socket creation) and the    */
/*  DNS hook in core/io/ip.cpp pull the FD and a shared mutex from here.  */
/*                                                                        */
/*  Wire used by every request: a length-prefixed framed message via      */
/*  TGFDPassing::send_msg / recv_msg. Layout described next to the        */
/*  request builder in this file's .cpp.                                  */
/**************************************************************************/

#pragma once

#include "broker_protocol.h"

#include "core/error/error_list.h"
#include "core/io/ip.h"
#include "core/io/ip_address.h"
#include "core/string/ustring.h"

class RendererNetClient {
public:
	// Adopts the inherited control handle as the singleton's broker channel.
	// intptr_t round-trips both a POSIX FD (small int) and a Windows HANDLE
	// (pointer-sized) without lossy casting. Refuses re-installation: if a
	// different handle is offered after one is already in place, returns
	// ERR_ALREADY_EXISTS so the caller can't silently leak it.
	static Error install(intptr_t p_handle);
	static void shutdown();
	static bool is_installed();

	// Asks the broker to open a connected TCP/UDP socket to the given
	// destination. On success the returned FD is owned by the caller.
	// `p_hostname` may be empty if the caller already has the IP.
	static Error open_socket(BrokerProtocol::Opcode p_opcode, const IPAddress &p_ip, uint16_t p_port,
			const String &p_hostname, int *r_fd);

	// Asks the broker to resolve a hostname. Returns up to 8 IPs. `false`
	// on broker error so callers can fall through to a fail-closed path.
	static bool resolve_hostname(const String &p_hostname, IP::Type p_type,
			List<IPAddress> &r_addresses);
};
