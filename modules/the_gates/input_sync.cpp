/**************************************************************************/
/*  input_sync.cpp                                                        */
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

#include "input_sync.h"
#include "core/input/input.h"
#include "variant_tools.h"
#include "zmq_context.h"

void InputSync::socket_bind(const String &p_address) {
	sock.bind(tg_resolve_ipc_address(p_address).utf8().get_data());
}

void InputSync::socket_connect(const String &p_address) {
	sock.connect(tg_resolve_ipc_address(p_address).utf8().get_data());
}

void InputSync::send_input_event(const Ref<InputEvent> &p_event) {
	std::string msg_str = var_to_str(p_event).utf8().get_data();
	zmq::message_t msg(msg_str);
	auto result = sock.send(msg, zmq::send_flags::none);
	if (!result) {
		print_line("Failed to send input event");
	}
}

void InputSync::receive_input_events() {
	zmq::message_t msg;

	while (sock.recv(msg, zmq::recv_flags::dontwait)) {
		std::string msg_str(static_cast<const char *>(msg.data()), msg.size());
		Input::get_singleton()->parse_input_event((Ref<InputEvent>)str_to_var(msg_str.c_str()));
	}
}

void InputSync::close() {
	sock.close();
}

void InputSync::_bind_methods() {
	ClassDB::bind_method(D_METHOD("socket_bind", "address"), &InputSync::socket_bind, DEFVAL(INPUT_SYNC_ADDRESS));
	ClassDB::bind_method(D_METHOD("socket_connect", "address"), &InputSync::socket_connect, DEFVAL(INPUT_SYNC_ADDRESS));
	ClassDB::bind_method(D_METHOD("send_input_event", "event"), &InputSync::send_input_event);
	ClassDB::bind_method(D_METHOD("close"), &InputSync::close);
}

InputSync::InputSync() :
		sock(ctx, zmq::socket_type::pair) {
}

InputSync::~InputSync() {
}
