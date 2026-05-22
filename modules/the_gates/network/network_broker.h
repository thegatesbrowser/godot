/**************************************************************************/
/*  network_broker.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/*                                                                        */
/*  Launcher-side service that mediates every socket the renderer opens. */
/*  Owned by `Sandbox`; not GDScript-visible.                             */
/*                                                                        */
/*  The launcher creates a pre-connected control channel before spawning  */
/*  the renderer (AF_UNIX socketpair on POSIX, paired named-pipe handles  */
/*  on Windows). The renderer side of the pair is inherited into the      */
/*  spawned child; the launcher passes it to `start()` so the broker      */
/*  reads requests on its half of the same pair. There is no filesystem   */
/*  rendezvous and no `accept()` loop.                                    */
/*                                                                        */
/*  Per request the broker:                                               */
/*    1. resolves the hostname if any (renderer cannot do DNS itself);    */
/*    2. checks the resolved IP against CIDRPolicy;                       */
/*    3. opens a kernel socket and connects/binds it;                     */
/*    4. ships the FD back via SCM_RIGHTS (POSIX) or WSADuplicateSocket   */
/*       (Windows) plus a small status payload.                           */
/*                                                                        */
/*  After the FD is in the renderer's process, all read/write/poll calls  */
/*  hit the kernel directly. Only socket creation and DNS resolution      */
/*  cross the broker — no per-byte overhead.                              */
/**************************************************************************/

#pragma once

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
