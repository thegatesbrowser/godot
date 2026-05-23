/**************************************************************************/
/*  network_broker.h                                                      */
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

#include "broker_protocol.h"

#include "core/object/ref_counted.h"
#include "core/os/thread.h"
#include "core/string/ustring.h"

#include <atomic>

class NetworkBroker : public RefCounted {
	GDCLASS(NetworkBroker, RefCounted);

	// intptr_t to losslessly hold either a POSIX FD or a Windows HANDLE.
	// Sentinel for "not started" is -1 (POSIX) / INVALID_HANDLE_VALUE on
	// Windows fits in intptr_t — we use -1 as the sentinel everywhere.
	intptr_t peer_handle = -1;
	void *target_process_handle = nullptr;
	Thread service_thread;
	std::atomic<bool> shutdown_requested{ false };

	struct Stats {
		std::atomic<uint64_t> requests_total{ 0 };
		std::atomic<uint64_t> requests_allowed{ 0 };
		std::atomic<uint64_t> requests_denied{ 0 };
		std::atomic<uint64_t> dns_resolutions{ 0 };
	} stats;

	static void _service_thread_func(void *p_userdata);
	void _serve();

	bool _recv_request(BrokerProtocol::Request &r_req);
	Error _send_response(const BrokerProtocol::Response &p_resp, int p_fd);
	BrokerProtocol::Response _handle_resolve(const BrokerProtocol::Request &p_req);
	BrokerProtocol::Response _handle_open_socket(const BrokerProtocol::Request &p_req, int *r_fd);

public:
	// Windows only: the renderer-process HANDLE passed to WSADuplicateSocket
	// so the FD lands in the right address space. The broker duplicates the
	// handle so its lifetime is independent of the caller's copy; the
	// duplicate is closed in `shutdown`. Ignored on POSIX.
	void set_target_process_handle(void *p_handle);

	// Starts the service thread on a pre-bound peer handle (launcher side of
	// the socketpair / pipe pair). The broker owns the handle from this
	// point on and closes it during shutdown.
	Error start(intptr_t p_peer_handle);

	// Signals the broker thread to exit; joins. Idempotent.
	void shutdown();

	// Non-blocking half of `shutdown`: closes the peer FD and signals the
	// loop, but does not join. For callers that can't afford to block on
	// in-flight `getaddrinfo` / `::connect`; see `Sandbox::stop_broker`.
	// Idempotent.
	void request_shutdown();

	// Snapshot for --network-diagnostic and support tooling.
	Dictionary state() const;

	NetworkBroker() = default;
	~NetworkBroker() override;
};
