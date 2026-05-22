/**************************************************************************/
/*  renderer_net_client.cpp                                               */
/**************************************************************************/

#include "renderer_net_client.h"

#include "broker_protocol.h"
#include "fd_passing.h"

#include "core/os/mutex.h"

#ifdef WINDOWS_ENABLED
#include <winsock2.h>
#include <windows.h>
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

Error send_request(const BrokerProtocol::Request &p_req,
		int *r_received_fd, uint8_t *r_resp, int p_resp_capacity, int *r_resp_len) {
	uint8_t req_buf[BrokerProtocol::Request::MAX_SIZE];
	const int req_len = p_req.serialize(req_buf);

	MutexLock lock(s_mutex);

	const Error r = TGFDPassing::send_msg(s_control_handle, -1, nullptr, req_buf, req_len);
	if (r != OK) {
		return r;
	}
	return TGFDPassing::recv_msg(s_control_handle, r_received_fd, r_resp, p_resp_capacity, r_resp_len);
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

	uint8_t resp[16] = {};
	int resp_len = 0;
	int recv_fd = -1;
	const Error r = send_request(req, &recv_fd, resp, (int)sizeof(resp), &resp_len);
	if (r != OK) {
		if (recv_fd >= 0) {
			TG_CLOSE_NATIVE(recv_fd);
		}
		return r;
	}
	if (resp_len < 1 || resp[0] != BrokerProtocol::ST_OK) {
		if (recv_fd >= 0) {
			TG_CLOSE_NATIVE(recv_fd);
		}
		if (resp_len >= 1 &&
				(resp[0] == BrokerProtocol::ST_DENIED_POLICY ||
						resp[0] == BrokerProtocol::ST_DENIED_FORCE_FAIL)) {
			return ERR_UNAUTHORIZED;
		}
		return FAILED;
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

	uint8_t resp[2 + 16 * 8] = {};
	int resp_len = 0;
	int unused_fd = -1;
	const Error r = send_request(req, &unused_fd, resp, (int)sizeof(resp), &resp_len);
	if (unused_fd >= 0) {
		TG_CLOSE_NATIVE(unused_fd);
	}
	if (r != OK || resp_len < 2 || resp[0] != BrokerProtocol::ST_OK) {
		return false;
	}
	const int count = resp[1];
	for (int i = 0; i < count && (2 + (i + 1) * 16) <= resp_len; ++i) {
		IPAddress ip;
		ip.set_ipv6(resp + 2 + i * 16);
		if (ip.is_valid()) {
			r_addresses.push_back(ip);
		}
	}
	return !r_addresses.is_empty();
}
