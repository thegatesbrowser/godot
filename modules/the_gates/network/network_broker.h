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

	int peer_fd = -1;
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
	// so the FD lands in the right address space. Ignored on POSIX.
	void set_target_process_handle(void *p_handle) { target_process_handle = p_handle; }

	// Starts the service thread on a pre-bound peer FD (launcher side of the
	// socketpair / pipe pair). The broker owns the FD from this point on
	// and closes it during shutdown.
	Error start(int p_peer_fd);

	// Signals the broker thread to exit; joins. Idempotent.
	void shutdown();

	// Snapshot for --network-diagnostic and support tooling.
	Dictionary state() const;

	NetworkBroker() = default;
	~NetworkBroker() override;
};
