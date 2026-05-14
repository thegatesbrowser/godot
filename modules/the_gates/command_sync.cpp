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
#include "variant_tools.h"

CommandSync *CommandSync::singleton = nullptr;

void CommandSync::socket_bind(const String &p_address) {
	socket.bind(p_address);
}

void CommandSync::socket_connect(const String &p_address) {
	socket.connect(p_address);
}

void CommandSync::send_command(const Ref<Command> &p_command) {
	const CharString utf8 = var_to_str(p_command).utf8();
	Vector<uint8_t> payload;
	payload.resize(utf8.length());
	memcpy(payload.ptrw(), utf8.get_data(), utf8.length());
	socket.queue_message(payload);
	socket.poll();
}

void CommandSync::send_command(const String &p_name) {
	Ref<Command> command;
	command.instantiate();
	command->set_name(p_name);
	send_command(command);
}

void CommandSync::send_command(const String &p_name, const Array &p_args) {
	Ref<Command> command;
	command.instantiate();
	command->set_name(p_name);
	command->set_args(p_args);
	send_command(command);
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
	socket.poll();

	Vector<uint8_t> msg;
	while (socket.pop_message(msg)) {
		const String text = String::utf8((const char *)msg.ptr(), msg.size());
		call_execute_function((Ref<Command>)str_to_var(text));
	}
}

void CommandSync::close() {
	socket.close();
}

void CommandSync::_bind_methods() {
	ClassDB::bind_method(D_METHOD("socket_bind", "address"), &CommandSync::socket_bind, DEFVAL(COMMAND_SYNC_ADDRESS));
	ClassDB::bind_method(D_METHOD("receive_commands"), &CommandSync::receive_commands);

	ClassDB::bind_method(D_METHOD("get_execute_function"), &CommandSync::get_execute_function);
	ClassDB::bind_method(D_METHOD("set_execute_function", "execute_function"), &CommandSync::set_execute_function);
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "execute_function", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_execute_function", "get_execute_function");

	ClassDB::bind_method(D_METHOD("close"), &CommandSync::close);
}

CommandSync::CommandSync() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

CommandSync::~CommandSync() {
}
