/**************************************************************************/
/*  fd_passing.h                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/*                                                                        */
/*  Cross-platform message + FD transport between the launcher (broker)   */
/*  and the renderer (sandbox target) over a single pre-existing control  */
/*  channel.                                                              */
/*                                                                        */
/*  Wire format is one length-prefixed message per call. Each message     */
/*  carries a small byte payload plus, optionally, a single kernel file   */
/*  descriptor (TCP/UDP socket).                                          */
/*                                                                        */
/*    POSIX:   AF_UNIX SOCK_STREAM socketpair created by the launcher     */
/*             before posix_spawn. Child inherits one end via             */
/*             posix_spawn_file_actions_adddup2 at FD 3. FDs travel via   */
/*             SCM_RIGHTS ancillary data; payload via msg_iov.            */
/*                                                                        */
/*    Windows: anonymous named pipe pair, inheritable end handed to the   */
/*             child via Chromium sandbox's TargetPolicy::AddHandleToShare*/
/*             (TODO). FDs travel as inline WSAPROTOCOL_INFO bytes inside */
/*             the payload; the receiver materializes a SOCKET via        */
/*             WSASocketW(FROM_PROTOCOL_INFO).                            */
/**************************************************************************/

#pragma once

#include "core/error/error_list.h"

#include <stdint.h>

class TGFDPassing {
public:
	// Hard cap on the size of a single framed message. Every broker
	// message today is well under 256 bytes (DNS response is the largest
	// at 2 + 16*8); a larger length on the wire is either corruption or
	// a peer trying to DoS us with a multi-GB drain. Used by both POSIX
	// and Windows implementations.
	static constexpr uint32_t MAX_FRAME_BYTES = 65536;

	// Send one framed message on the control channel. If `p_payload_fd >= 0`,
	// the kernel FD is transferred to the peer alongside the payload bytes.
	// `p_target_handle` is the Windows renderer-process HANDLE used by
	// WSADuplicateSocket; ignored on POSIX. Pass `nullptr` if unused.
	static Error send_msg(int p_control_fd, int p_payload_fd,
			void *p_target_handle,
			const uint8_t *p_payload, int p_payload_len);

	// Receive one framed message. `*r_received_fd` is set to -1 when the
	// peer did not attach a kernel FD. The caller owns the returned FD on
	// success and must close it (or hand it off) when done.
	//
	// `r_payload_buf` must be at least as large as the expected user payload;
	// `*r_payload_len` is set to the number of user payload bytes written.
	// `ERR_OUT_OF_MEMORY` is returned if the message exceeds the buffer.
	static Error recv_msg(int p_control_fd,
			int *r_received_fd,
			uint8_t *r_payload_buf, int p_payload_buf_capacity,
			int *r_payload_len);
};
