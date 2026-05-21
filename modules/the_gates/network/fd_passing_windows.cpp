/**************************************************************************/
/*  fd_passing_windows.cpp                                                */
/**************************************************************************/
/*  Windows FD-passing over an inherited named-pipe handle. SOCKET        */
/*  handles travel as inline WSAPROTOCOL_INFO bytes appended to the user  */
/*  payload; the receiver calls WSASocketW(FROM_PROTOCOL_INFO, ...) to    */
/*  materialize a SOCKET equivalent to one created locally (Microsoft     */
/*  Learn: "Shared Sockets"). The pipe runs in byte mode so a length-     */
/*  prefixed framing layer applies identically to the POSIX variant.      */
/**************************************************************************/

#include "fd_passing.h"

#ifdef WINDOWS_ENABLED

#include "core/string/print_string.h"

#include <stdint.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace {

Error gle_to_error(DWORD err) {
	switch (err) {
		case ERROR_ACCESS_DENIED:
			return ERR_UNAUTHORIZED;
		case ERROR_PIPE_BUSY:
		case ERROR_FILE_NOT_FOUND:
		case ERROR_BROKEN_PIPE:
			return ERR_CANT_CONNECT;
		case ERROR_IO_PENDING:
			return ERR_BUSY;
		default:
			return FAILED;
	}
}

Error write_exact(HANDLE p_pipe, const uint8_t *p_buf, int p_len) {
	int sent = 0;
	while (sent < p_len) {
		DWORD chunk = 0;
		if (!::WriteFile(p_pipe, p_buf + sent, (DWORD)(p_len - sent), &chunk, nullptr)) {
			return gle_to_error(::GetLastError());
		}
		if (chunk == 0) {
			return ERR_CANT_CONNECT;
		}
		sent += (int)chunk;
	}
	return OK;
}

Error read_exact(HANDLE p_pipe, uint8_t *p_buf, int p_len) {
	int got = 0;
	while (got < p_len) {
		DWORD chunk = 0;
		if (!::ReadFile(p_pipe, p_buf + got, (DWORD)(p_len - got), &chunk, nullptr)) {
			return gle_to_error(::GetLastError());
		}
		if (chunk == 0) {
			return ERR_CANT_CONNECT;
		}
		got += (int)chunk;
	}
	return OK;
}

} // namespace

// Wire format on Windows (pipe in byte mode):
//   [u32 length BE][u8 has_fd][user payload][WSAPROTOCOL_INFOW if has_fd]
// `length` covers everything after itself.
Error TGFDPassing::send_msg(int p_control_fd, int p_payload_fd,
		void *p_target_handle, const uint8_t *p_payload, int p_payload_len) {
	if (p_control_fd < 0 || p_payload_len < 0) {
		return ERR_INVALID_PARAMETER;
	}
	const HANDLE pipe = (HANDLE)(intptr_t)p_control_fd;
	const HANDLE target_proc = (HANDLE)p_target_handle;

	const bool has_fd = (p_payload_fd >= 0) && (p_payload_fd != (int)INVALID_SOCKET);
	WSAPROTOCOL_INFOW info = {};
	if (has_fd) {
		if (target_proc == nullptr) {
			return ERR_INVALID_PARAMETER;
		}
		const DWORD target_pid = ::GetProcessId(target_proc);
		if (::WSADuplicateSocketW((SOCKET)p_payload_fd, target_pid, &info) != 0) {
			return gle_to_error(::WSAGetLastError());
		}
	}

	const uint32_t body_len = (uint32_t)(1 + p_payload_len + (has_fd ? (int)sizeof(WSAPROTOCOL_INFOW) : 0));
	uint8_t length_be[4] = {
		(uint8_t)((body_len >> 24) & 0xFF),
		(uint8_t)((body_len >> 16) & 0xFF),
		(uint8_t)((body_len >> 8) & 0xFF),
		(uint8_t)(body_len & 0xFF),
	};
	const Error e1 = write_exact(pipe, length_be, (int)sizeof(length_be));
	if (e1 != OK) {
		return e1;
	}
	const uint8_t flag = has_fd ? (uint8_t)1 : (uint8_t)0;
	const Error e2 = write_exact(pipe, &flag, 1);
	if (e2 != OK) {
		return e2;
	}
	if (p_payload_len > 0) {
		const Error e3 = write_exact(pipe, p_payload, p_payload_len);
		if (e3 != OK) {
			return e3;
		}
	}
	if (has_fd) {
		const Error e4 = write_exact(pipe, reinterpret_cast<const uint8_t *>(&info), (int)sizeof(info));
		if (e4 != OK) {
			return e4;
		}
	}
	return OK;
}

Error TGFDPassing::recv_msg(int p_control_fd, int *r_received_fd,
		uint8_t *r_payload_buf, int p_payload_buf_capacity, int *r_payload_len) {
	if (p_control_fd < 0 || r_received_fd == nullptr || r_payload_len == nullptr) {
		return ERR_INVALID_PARAMETER;
	}
	*r_received_fd = -1;
	*r_payload_len = 0;
	const HANDLE pipe = (HANDLE)(intptr_t)p_control_fd;

	uint8_t length_be[4] = {};
	Error e = read_exact(pipe, length_be, (int)sizeof(length_be));
	if (e != OK) {
		return e;
	}
	const uint32_t body_len = ((uint32_t)length_be[0] << 24) |
			((uint32_t)length_be[1] << 16) |
			((uint32_t)length_be[2] << 8) |
			(uint32_t)length_be[3];
	if (body_len < 1) {
		return FAILED;
	}
	if (body_len > MAX_FRAME_BYTES) {
		return ERR_OUT_OF_MEMORY;
	}

	uint8_t flag = 0;
	e = read_exact(pipe, &flag, 1);
	if (e != OK) {
		return e;
	}
	const bool has_fd = (flag != 0);
	const uint32_t fd_blob = has_fd ? (uint32_t)sizeof(WSAPROTOCOL_INFOW) : 0;
	if (body_len < 1 + fd_blob) {
		return FAILED;
	}
	const uint32_t user_len = body_len - 1 - fd_blob;

	if ((int)user_len > p_payload_buf_capacity) {
		uint8_t scratch[256];
		uint32_t remaining = body_len - 1;
		while (remaining > 0) {
			const uint32_t chunk = remaining > sizeof(scratch) ? (uint32_t)sizeof(scratch) : remaining;
			e = read_exact(pipe, scratch, (int)chunk);
			if (e != OK) {
				return e;
			}
			remaining -= chunk;
		}
		return ERR_OUT_OF_MEMORY;
	}
	if (user_len > 0) {
		e = read_exact(pipe, r_payload_buf, (int)user_len);
		if (e != OK) {
			return e;
		}
	}
	*r_payload_len = (int)user_len;
	if (has_fd) {
		WSAPROTOCOL_INFOW info = {};
		e = read_exact(pipe, reinterpret_cast<uint8_t *>(&info), (int)sizeof(info));
		if (e != OK) {
			return e;
		}
		const SOCKET sock = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
				FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_OVERLAPPED);
		if (sock == INVALID_SOCKET) {
			return gle_to_error(::WSAGetLastError());
		}
		*r_received_fd = (int)sock;
	}
	return OK;
}

#endif // WINDOWS_ENABLED
