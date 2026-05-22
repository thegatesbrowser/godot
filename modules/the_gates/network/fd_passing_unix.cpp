/**************************************************************************/
/*  fd_passing_unix.cpp                                                   */
/**************************************************************************/
/*  SCM_RIGHTS-based FD passing on AF_UNIX SOCK_STREAM sockets. Both      */
/*  macOS and Linux share this implementation.                            */
/**************************************************************************/

#include "fd_passing.h"

#if !defined(WINDOWS_ENABLED) && (defined(UNIX_ENABLED) || defined(MACOS_ENABLED) || defined(LINUXBSD_ENABLED))

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

Error errno_to_error(int err) {
	if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
		return ERR_BUSY;
	}
	if (err == EACCES) {
		return ERR_UNAUTHORIZED;
	}
	if (err == ECONNREFUSED || err == ENOENT || err == EPIPE) {
		return ERR_CANT_CONNECT;
	}
	return FAILED;
}

Error write_exact(int p_fd, const uint8_t *p_buf, int p_len) {
	int sent = 0;
	while (sent < p_len) {
		const ssize_t n = ::send(p_fd, p_buf + sent, (size_t)(p_len - sent), 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno_to_error(errno);
		}
		if (n == 0) {
			return ERR_CANT_CONNECT;
		}
		sent += (int)n;
	}
	return OK;
}

Error read_exact(int p_fd, uint8_t *p_buf, int p_len) {
	int got = 0;
	while (got < p_len) {
		const ssize_t n = ::recv(p_fd, p_buf + got, (size_t)(p_len - got), 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno_to_error(errno);
		}
		if (n == 0) {
			return ERR_CANT_CONNECT;
		}
		got += (int)n;
	}
	return OK;
}

void encode_length(uint8_t *p_out, uint32_t p_len) {
	p_out[0] = (uint8_t)((p_len >> 24) & 0xFF);
	p_out[1] = (uint8_t)((p_len >> 16) & 0xFF);
	p_out[2] = (uint8_t)((p_len >> 8) & 0xFF);
	p_out[3] = (uint8_t)(p_len & 0xFF);
}

uint32_t decode_length(const uint8_t *p_in) {
	return ((uint32_t)p_in[0] << 24) | ((uint32_t)p_in[1] << 16) |
			((uint32_t)p_in[2] << 8) | (uint32_t)p_in[3];
}

} // namespace

// Wire format on AF_UNIX SOCK_STREAM:
//   [u32 big-endian length][payload bytes]
// Optional kernel FD travels via SCM_RIGHTS ancillary attached to the
// sendmsg that carries (at least the start of) the message. SOCK_STREAM
// coalesces, so receivers split the stream into messages purely by the
// length prefix.
Error TGFDPassing::send_msg(intptr_t p_control_handle, int p_payload_fd,
		void * /*p_target_handle*/, const uint8_t *p_payload, int p_payload_len) {
	const int p_control_fd = (int)p_control_handle;
	if (p_control_fd < 0 || p_payload_len < 0) {
		return ERR_INVALID_PARAMETER;
	}
	if ((uint32_t)p_payload_len > MAX_FRAME_BYTES) {
		return ERR_INVALID_PARAMETER;
	}

	uint8_t length_be[4];
	encode_length(length_be, (uint32_t)p_payload_len);

	struct iovec iov[2];
	iov[0].iov_base = length_be;
	iov[0].iov_len = sizeof(length_be);
	iov[1].iov_base = (void *)p_payload;
	iov[1].iov_len = (size_t)p_payload_len;

	struct msghdr msg = {};
	msg.msg_iov = iov;
	msg.msg_iovlen = (p_payload_len > 0) ? 2 : 1;

	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} cmsg_buf = {};

	if (p_payload_fd >= 0) {
		msg.msg_control = cmsg_buf.buf;
		msg.msg_controllen = sizeof(cmsg_buf.buf);
		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &p_payload_fd, sizeof(int));
	}

	// SOCK_STREAM ancillary data only rides the first sendmsg of a message,
	// so we issue exactly one sendmsg here. If it returns a partial write,
	// the ancillary has already been delivered with whatever bytes did go
	// through; we finish the remainder with plain send().
	for (;;) {
		const ssize_t sent = ::sendmsg(p_control_fd, &msg, 0);
		if (sent < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno_to_error(errno);
		}
		if (sent == 0) {
			return ERR_CANT_CONNECT;
		}
		const int total = (int)sizeof(length_be) + p_payload_len;
		if ((int)sent == total) {
			return OK;
		}
		// Partial write: catch up the tail across header / payload boundary.
		int remaining = total - (int)sent;
		int cursor = (int)sent;
		if (cursor < (int)sizeof(length_be)) {
			const int hdr_left = (int)sizeof(length_be) - cursor;
			const Error e = write_exact(p_control_fd, length_be + cursor, hdr_left);
			if (e != OK) {
				return e;
			}
			cursor += hdr_left;
			remaining -= hdr_left;
		}
		if (remaining > 0) {
			const int payload_offset = cursor - (int)sizeof(length_be);
			return write_exact(p_control_fd, p_payload + payload_offset, remaining);
		}
		return OK;
	}
}

Error TGFDPassing::recv_msg(intptr_t p_control_handle, int *r_received_fd,
		uint8_t *r_payload_buf, int p_payload_buf_capacity, int *r_payload_len) {
	const int p_control_fd = (int)p_control_handle;
	if (p_control_fd < 0 || r_received_fd == nullptr || r_payload_len == nullptr) {
		return ERR_INVALID_PARAMETER;
	}

	*r_received_fd = -1;
	*r_payload_len = 0;

	// One recvmsg pulls whatever the kernel has of the message head, plus
	// any ancillary FD attached to it. POSIX guarantees the SCM_RIGHTS
	// arrives with the first recvmsg that returns any bytes of the message,
	// so the read_exact() loop below can use plain recv() for the rest.
	uint8_t length_be[4] = {};
	struct iovec iov = {};
	iov.iov_base = length_be;
	iov.iov_len = sizeof(length_be);

	struct msghdr msg = {};
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} cmsg_buf = {};
	msg.msg_control = cmsg_buf.buf;
	msg.msg_controllen = sizeof(cmsg_buf.buf);

	ssize_t first_recv = 0;
	for (;;) {
		first_recv = ::recvmsg(p_control_fd, &msg, 0);
		if (first_recv >= 0) {
			break;
		}
		if (errno == EINTR) {
			continue;
		}
		return errno_to_error(errno);
	}
	if (first_recv == 0) {
		return ERR_CANT_CONNECT;
	}

	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
			cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
			int got_fd = -1;
			memcpy(&got_fd, CMSG_DATA(cmsg), sizeof(int));
			*r_received_fd = got_fd;
		}
	}

	// Finish reading the length header if the first recvmsg didn't get all 4.
	if (first_recv < (ssize_t)sizeof(length_be)) {
		const Error e = read_exact(p_control_fd, length_be + first_recv,
				(int)sizeof(length_be) - (int)first_recv);
		if (e != OK) {
			if (*r_received_fd >= 0) {
				::close(*r_received_fd);
				*r_received_fd = -1;
			}
			return e;
		}
	}

	const uint32_t payload_len = decode_length(length_be);

	if (payload_len > MAX_FRAME_BYTES) {
		if (*r_received_fd >= 0) {
			::close(*r_received_fd);
			*r_received_fd = -1;
		}
		return ERR_OUT_OF_MEMORY;
	}
	if (payload_len == 0) {
		return OK;
	}
	if ((int)payload_len > p_payload_buf_capacity) {
		// Drain the body so the stream stays aligned, then refuse.
		uint8_t scratch[256];
		uint32_t remaining = payload_len;
		while (remaining > 0) {
			const uint32_t chunk = remaining > sizeof(scratch) ? (uint32_t)sizeof(scratch) : remaining;
			const Error e = read_exact(p_control_fd, scratch, (int)chunk);
			if (e != OK) {
				break;
			}
			remaining -= chunk;
		}
		if (*r_received_fd >= 0) {
			::close(*r_received_fd);
			*r_received_fd = -1;
		}
		return ERR_OUT_OF_MEMORY;
	}

	const Error e = read_exact(p_control_fd, r_payload_buf, (int)payload_len);
	if (e != OK) {
		if (*r_received_fd >= 0) {
			::close(*r_received_fd);
			*r_received_fd = -1;
		}
		return e;
	}
	*r_payload_len = (int)payload_len;
	return OK;
}

#endif // !WINDOWS_ENABLED && (UNIX_ENABLED || MACOS_ENABLED || LINUXBSD_ENABLED)
