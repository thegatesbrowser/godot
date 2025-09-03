/**************************************************************************/
/*  socket_peer_monitor.cpp                                               */
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

#include "socket_peer_monitor.h"
#include "thirdparty/zmqpp/message.hpp"
#include "zmq_context.h"

void SocketPeerMonitor::bind(const String &p_address, const String &p_monitor_endpoint) {
	sock.bind(p_address.utf8().get_data());

	std::string monitor_endpoint = p_monitor_endpoint.utf8().get_data();
	sock.monitor(monitor_endpoint, zmqpp::event::all);
	monitor_sock.connect(monitor_endpoint);
}

void SocketPeerMonitor::connect(const String &p_address, const String &p_monitor_endpoint) {
	sock.connect(p_address.utf8().get_data());

	std::string monitor_endpoint = p_monitor_endpoint.utf8().get_data();
	sock.monitor(monitor_endpoint, zmqpp::event::all);
	monitor_sock.connect(monitor_endpoint);
}

void SocketPeerMonitor::poll_monitor() {
	zmqpp::message msg;
	while (monitor_sock.receive(msg, true)) {
		if (msg.parts() >= 1) {
			std::string part0;
			msg.get(part0, 0);
			if (part0.size() >= 6) {
				const uint8_t *data = reinterpret_cast<const uint8_t *>(part0.data());
				uint16_t event_id = 0;
				uint32_t value = 0;
				memcpy(&event_id, data, sizeof(uint16_t));
				memcpy(&value, data + sizeof(uint16_t), sizeof(uint32_t));
				if (event_id == ZMQ_EVENT_DISCONNECTED || event_id == ZMQ_EVENT_CLOSED || event_id == ZMQ_EVENT_CLOSE_FAILED) {
					peer_disconnected = true;
				}
				if (event_id == ZMQ_EVENT_CONNECTED || event_id == ZMQ_EVENT_ACCEPTED || event_id == ZMQ_EVENT_LISTENING) {
					peer_disconnected = false;
				}
			}
		}
	}
}

void SocketPeerMonitor::close() {
	sock.close();
	monitor_sock.close();
}

void SocketPeerMonitor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bind", "address", "monitor_endpoint"), &SocketPeerMonitor::bind, DEFVAL(SOCKET_PEER_MONITOR_ADDRESS), DEFVAL(SOCKET_PEER_MONITOR_PARENT_ENDPOINT));
	ClassDB::bind_method(D_METHOD("connect", "address", "monitor_endpoint"), &SocketPeerMonitor::connect, DEFVAL(SOCKET_PEER_MONITOR_ADDRESS), DEFVAL(SOCKET_PEER_MONITOR_CHILD_ENDPOINT));
	ClassDB::bind_method(D_METHOD("poll_monitor"), &SocketPeerMonitor::poll_monitor);
	ClassDB::bind_method(D_METHOD("is_peer_connected"), &SocketPeerMonitor::is_peer_connected);
	ClassDB::bind_method(D_METHOD("close"), &SocketPeerMonitor::close);
}

SocketPeerMonitor::SocketPeerMonitor(zmqpp::socket_type type) :
		sock(zmqpp::socket(ctx, type)), monitor_sock(zmqpp::socket(ctx, zmqpp::socket_type::pair)) {
}

SocketPeerMonitor::~SocketPeerMonitor() {
}
