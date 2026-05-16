/**************************************************************************/
/*  command_sync.cpp                                                      */
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

#include "command_sync.h"
#include "core/input/input.h"
#include "socket_acl_win.h"
#include "variant_tools.h"
#include "zmq_context.h"
#include <zmq.h>

CommandSync *CommandSync::singleton = nullptr;

void CommandSync::socket_bind(const String &p_address) {
	const String resolved = tg_resolve_ipc_address(p_address);
	sock.bind(resolved.utf8().get_data());
	tg_apply_untrusted_acl(resolved);
}

void CommandSync::socket_connect(const String &p_address, const String &p_monitor_endpoint) {
	sock.connect(tg_resolve_ipc_address(p_address).utf8().get_data());

	std::string monitor_endpoint = p_monitor_endpoint.utf8().get_data();
	zmq_socket_monitor(sock.handle(), monitor_endpoint.c_str(), ZMQ_EVENT_ALL);
	monitor_sock.connect(monitor_endpoint);
}

void CommandSync::send_command(const Ref<Command> &p_command) {
	std::string msg_str = var_to_str(p_command).utf8().get_data();
	zmq::message_t msg(msg_str);
	if (!sock.send(msg, zmq::send_flags::none)) {
		print_line("Failed to send command");
	}
}

void CommandSync::send_command(const String &p_name) {
	Command *command = memnew(Command);
	command->set_name(p_name);
	send_command(Ref<Command>(command));
}

void CommandSync::send_command(const String &p_name, const Array &p_args) {
	Command *command = memnew(Command);
	command->set_name(p_name);
	command->set_args(p_args);
	send_command(Ref<Command>(command));
}

void CommandSync::poll_monitor() {
	zmq::message_t msg;

	while (monitor_sock.recv(msg, zmq::recv_flags::dontwait)) {
		if (msg.size() >= 6) {
			const uint8_t *msg_data = static_cast<const uint8_t *>(msg.data());
			uint16_t event_id = 0;
			uint32_t value = 0;
			memcpy(&event_id, msg_data, sizeof(uint16_t));
			memcpy(&value, msg_data + sizeof(uint16_t), sizeof(uint32_t));

			print_line(vformat("ZMQ Monitor Event: %d, Value: %d", event_id, value));

			if (event_id == ZMQ_EVENT_DISCONNECTED || event_id == ZMQ_EVENT_CLOSED || event_id == ZMQ_EVENT_CLOSE_FAILED) {
				peer_disconnected = true;
				print_line("ZMQ Peer disconnected detected");
			}

			if (event_id == ZMQ_EVENT_CONNECTED || event_id == ZMQ_EVENT_ACCEPTED || event_id == ZMQ_EVENT_LISTENING) {
				peer_disconnected = false;
				print_line("ZMQ Peer connected detected");
			}
		}
	}
}

Variant CommandSync::call_execute_function(const Ref<Command> &p_command) {
	ERR_FAIL_COND_V_MSG(!execute_function.is_valid(), Variant(), "CommandSync requires a valid 'execute_function'");

	Array argv;
	argv.append(p_command);
	Variant ret = execute_function.callv(argv);
	ERR_FAIL_COND_V_MSG(ret.get_type() == Variant::NIL, Variant(), "Failed to execute function");

	return ret;
}

void CommandSync::bind_commands() {
	auto _input_set_mouse_mode = [](Input::MouseMode p_mode) {
		print_line("_input_set_mouse_mode " + itos((int)p_mode));
		DisplayServer::_input_set_mouse_mode(p_mode);

		Array args;
		args.append(p_mode);
		singleton->send_command("set_mouse_mode", args);
	};
	Input::set_mouse_mode_func = _input_set_mouse_mode;

	auto _scene_tree_send_command = [](const String &p_name, const Array &p_args) {
		singleton->send_command(p_name, p_args);
	};
	SceneTree::send_command_func = _scene_tree_send_command;
}

void CommandSync::receive_commands() {
	zmq::message_t msg;

	while (sock.recv(msg, zmq::recv_flags::dontwait)) {
		std::string msg_str(static_cast<const char *>(msg.data()), msg.size());
		Variant res = call_execute_function((Ref<Command>)str_to_var(msg_str.c_str()));
	}
}

void CommandSync::close() {
	sock.close();
	monitor_sock.close();
}

void CommandSync::_bind_methods() {
	ClassDB::bind_method(D_METHOD("socket_bind", "address"), &CommandSync::socket_bind, DEFVAL(COMMAND_SYNC_ADDRESS));
	ClassDB::bind_method(D_METHOD("socket_connect", "address", "monitor_endpoint"), &CommandSync::socket_connect, DEFVAL(COMMAND_SYNC_ADDRESS), DEFVAL(COMMAND_SYNC_MONITOR_ENDPOINT));
	ClassDB::bind_method(D_METHOD("receive_commands"), &CommandSync::receive_commands);

	ClassDB::bind_method(D_METHOD("get_execute_function"), &CommandSync::get_execute_function);
	ClassDB::bind_method(D_METHOD("set_execute_function", "execute_function"), &CommandSync::set_execute_function);
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "execute_function", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_execute_function", "get_execute_function");

	ClassDB::bind_method(D_METHOD("close"), &CommandSync::close);
}

CommandSync::CommandSync(zmq::socket_type type, zmq::socket_type monitor_type) :
		sock(ctx, type), monitor_sock(ctx, monitor_type) {
	if (singleton == nullptr) {
		singleton = this;
	}
}

CommandSync::~CommandSync() {
}
