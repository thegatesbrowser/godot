/**************************************************************************/
/*  socket_peer_monitor.h                                                 */
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

#ifndef SOCKET_PEER_MONITOR_H
#define SOCKET_PEER_MONITOR_H

#include "scene/main/node.h"
#include "thirdparty/zmqpp/socket.hpp"

#ifdef WINDOWS_ENABLED
static const String SOCKET_PEER_MONITOR_ADDRESS("ipc://sandbox/socket_peer_monitor");
#else
static const String SOCKET_PEER_MONITOR_ADDRESS("ipc:///tmp/socket_peer_monitor");
#endif

static const String SOCKET_PEER_MONITOR_PARENT_ENDPOINT("inproc://socket_peer_monitor.parent");
static const String SOCKET_PEER_MONITOR_CHILD_ENDPOINT("inproc://socket_peer_monitor.child");

class SocketPeerMonitor : public Node {
	GDCLASS(SocketPeerMonitor, Node);

	zmqpp::socket sock;
	zmqpp::socket monitor_sock;
	bool peer_disconnected = false;

protected:
	static void _bind_methods();

public:
	void bind(const String &p_address = SOCKET_PEER_MONITOR_ADDRESS, const String &p_monitor_endpoint = SOCKET_PEER_MONITOR_PARENT_ENDPOINT);
	void connect(const String &p_address = SOCKET_PEER_MONITOR_ADDRESS, const String &p_monitor_endpoint = SOCKET_PEER_MONITOR_CHILD_ENDPOINT);

	void poll_monitor();
	bool is_peer_connected() const { return !peer_disconnected; }

	void close();

	SocketPeerMonitor(zmqpp::socket_type type = zmqpp::socket_type::pair);
	~SocketPeerMonitor();
};

#endif // SOCKET_PEER_MONITOR_H
