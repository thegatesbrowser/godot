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

#include "core/core_string_names.h"
#include "core/input/input.h"
#include "variant_tools.h"

void InputSync::socket_bind(const String &p_address) {
	socket.bind(p_address);
}

void InputSync::socket_connect(const String &p_address) {
	socket.connect(p_address);
}

void InputSync::send_input_event(const Ref<InputEvent> &p_event) {
	std::string msg_str = var_to_str(p_event).utf8().get_data();
	Vector<uint8_t> payload;
	payload.resize(msg_str.size());
	if (!payload.is_empty()) {
		memcpy(payload.ptrw(), msg_str.data(), msg_str.size());
	}
	socket.queue_message(payload);
	if (!socket.poll()) {
		print_line("Failed to send input event");
	}
}

void InputSync::receive_input_events() {
	if (!socket.poll()) {
		return;
	}

	Vector<uint8_t> msg;
	while (socket.pop_message(msg)) {
		std::string msg_str;
		msg_str.resize(msg.size());
		if (!msg.is_empty()) {
			memcpy(msg_str.data(), msg.ptr(), msg.size());
		}
		Input::get_singleton()->parse_input_event((Ref<InputEvent>)str_to_var(msg_str.c_str()));
	}
}

void InputSync::close() {
	socket.close();
}

void InputSync::_bind_methods() {
	ClassDB::bind_method(D_METHOD("socket_bind", "address"), &InputSync::socket_bind, DEFVAL(INPUT_SYNC_ADDRESS));
	ClassDB::bind_method(D_METHOD("send_input_event", "event"), &InputSync::send_input_event);
	ClassDB::bind_method(D_METHOD("close"), &InputSync::close);
}

InputSync::InputSync() :
		socket() {
}

InputSync::~InputSync() {
}
