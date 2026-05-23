/**************************************************************************/
/*  renderer_net_client.cpp                                               */
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

#include "renderer_net_client.h"

#include "broker_protocol.h"
#include "fd_passing.h"

#include "core/os/mutex.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#include <winsock2.h>
#define TG_CLOSE_NATIVE(fd) ::closesocket((SOCKET)(fd))
#else
#include <unistd.h>
#define TG_CLOSE_NATIVE(fd) ::close(fd)
#endif

namespace {

// One control handle and one mutex serialize every request (socket-open + DNS)
// against the launcher-side broker thread. The broker is single-peer so we
// don't need per-call correlation IDs — replies arrive in order. intptr_t
// holds either a POSIX FD or a Windows HANDLE losslessly.
intptr_t s_control_handle = -1;
Mutex s_mutex;

void close_native_fd(int p_fd) {
	if (p_fd < 0) {
		return;
	}
	TG_CLOSE_NATIVE(p_fd);
}

// Locked send/receive round-trip. The returned Response covers status, errno,
// and any payload (resolved IPs or none). The kernel FD, if any, lands in
// r_received_fd.
Error round_trip(const BrokerProtocol::Request &p_req,
		BrokerProtocol::Response &r_resp, int *r_received_fd) {
	*r_received_fd = -1;

	uint8_t req_buf[BrokerProtocol::Request::MAX_SIZE];
	const int req_len = p_req.serialize(req_buf);

	MutexLock lock(s_mutex);

	const Error sent = TGFDPassing::send_msg(s_control_handle, -1, nullptr, req_buf, req_len);
	if (sent != OK) {
		return sent;
	}

	uint8_t resp_buf[BrokerProtocol::Response::MAX_SIZE];
	int resp_len = 0;
	const Error received = TGFDPassing::recv_msg(s_control_handle, r_received_fd,
			resp_buf, (int)sizeof(resp_buf), &resp_len);
	if (received != OK) {
		return received;
	}
	if (!r_resp.parse(resp_buf, resp_len)) {
		return ERR_INVALID_DATA;
	}
	return OK;
}

Error status_to_error(uint8_t p_status) {
	if (p_status == BrokerProtocol::ST_DENIED_POLICY ||
			p_status == BrokerProtocol::ST_DENIED_FORCE_FAIL) {
		return ERR_UNAUTHORIZED;
	}
	return FAILED;
}

} // namespace

Error RendererNetClient::install(intptr_t p_handle) {
	if (p_handle < 0) {
		return ERR_UNCONFIGURED;
	}
	if (s_control_handle >= 0) {
		if (s_control_handle == p_handle) {
			return OK;
		}
		// Different handle on a re-install — refuse so the caller can't
		// silently leak it; replacing in place would break the in-flight
		// broker thread on the other end.
		return ERR_ALREADY_EXISTS;
	}
	s_control_handle = p_handle;
	return OK;
}

void RendererNetClient::shutdown() {
	if (s_control_handle >= 0) {
#ifdef WINDOWS_ENABLED
		::CloseHandle((HANDLE)s_control_handle);
#else
		::close((int)s_control_handle);
#endif
		s_control_handle = -1;
	}
}

bool RendererNetClient::is_installed() {
	return s_control_handle >= 0;
}

Error RendererNetClient::open_socket(BrokerProtocol::Opcode p_opcode, const IPAddress &p_ip, uint16_t p_port,
		const String &p_hostname, int *r_fd) {
	if (r_fd == nullptr) {
		return ERR_INVALID_PARAMETER;
	}
	*r_fd = -1;
	if (s_control_handle < 0) {
		return ERR_UNCONFIGURED;
	}

	BrokerProtocol::Request req;
	req.opcode = (uint8_t)p_opcode;
	req.set_ip(p_ip);
	req.port = p_port;
	req.hostname = p_hostname;

	BrokerProtocol::Response resp;
	int recv_fd = -1;
	const Error rc = round_trip(req, resp, &recv_fd);
	if (rc != OK) {
		close_native_fd(recv_fd);
		return rc;
	}
	if (resp.status != BrokerProtocol::ST_OK) {
		close_native_fd(recv_fd);
		return status_to_error(resp.status);
	}
	if (recv_fd < 0) {
		return FAILED;
	}
	*r_fd = recv_fd;
	return OK;
}

bool RendererNetClient::resolve_hostname(const String &p_hostname, IP::Type /*p_type*/,
		List<IPAddress> &r_addresses) {
	if (s_control_handle < 0) {
		return false;
	}

	BrokerProtocol::Request req;
	req.opcode = (uint8_t)BrokerProtocol::OP_RESOLVE_HOSTNAME;
	req.hostname = p_hostname;

	BrokerProtocol::Response resp;
	int unused_fd = -1;
	const Error rc = round_trip(req, resp, &unused_fd);
	close_native_fd(unused_fd);
	if (rc != OK || resp.status != BrokerProtocol::ST_OK) {
		return false;
	}
	resp.get_resolved_ips(r_addresses);
	return !r_addresses.is_empty();
}
