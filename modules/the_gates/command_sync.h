/**************************************************************************/
/*  command_sync.h                                                        */
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

#ifndef COMMAND_SYNC_H
#define COMMAND_SYNC_H

#include "command.h"
#include "scene/main/node.h"
#include "thirdparty/zmqpp/socket.hpp"

#ifdef WINDOWS_ENABLED
static const String COMMAND_SYNC_ADDRESS("ipc://sandbox/command_sync");
#else
static const String COMMAND_SYNC_ADDRESS("ipc:///tmp/command_sync");
#endif

static const String COMMAND_SYNC_MONITOR_ENDPOINT("inproc://command_sync_monitor");

static const int COMMAND_SYNC_HEARTBEAT_IVL = 2000;
static const int COMMAND_SYNC_HEARTBEAT_TIMEOUT = 15000;

class CommandSync : public Node {
	GDCLASS(CommandSync, Node);

	static CommandSync *singleton;

	zmqpp::socket sock;
	Callable execute_function;

	// Monitoring
	zmqpp::socket monitor_sock;
	bool peer_disconnected = false;

protected:
	static void _bind_methods();

public:
	void socket_bind(const String &p_address = COMMAND_SYNC_ADDRESS);
	void socket_connect(const String &p_address = COMMAND_SYNC_ADDRESS, const String &p_monitor_endpoint = COMMAND_SYNC_MONITOR_ENDPOINT);

	void send_command(const Ref<Command> &p_command);
	void send_command(const String &p_name);
	void send_command(const String &p_name, const Array &p_args);

	// Monitor peer connection and report status.
	void poll_monitor();
	bool is_peer_connected() const { return !peer_disconnected; }

	Variant call_execute_function(const Ref<Command> &p_command);
	void set_execute_function(Callable p_execute_function) { execute_function = p_execute_function; }
	Callable get_execute_function() const { return execute_function; }

	void bind_commands();
	void receive_commands();

	void close();

	CommandSync(zmqpp::socket_type type = zmqpp::socket_type::pair, zmqpp::socket_type monitor_type = zmqpp::socket_type::pair);
	~CommandSync();
};

#endif // COMMAND_SYNC_H
