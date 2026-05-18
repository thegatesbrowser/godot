/**************************************************************************/
/*  renderer_lifecycle.cpp                                                */
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

#include "renderer_lifecycle.h"

#include "../ipc/command_sync.h"
#include "../ipc/external_texture.h"
#include "../ipc/input_sync.h"
#include "../ipc/zmq_runtime.h"
#include "../sandbox/sandbox.h"
#include "../sandbox/sandbox_diagnostics.h"

#include "core/config/engine.h"
#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/variant/array.h"
#include "servers/display_server.h"
#include "servers/rendering/rendering_device.h"

namespace {

CommandSync *command_sync = nullptr;
TGExternalTexture *ext_texture = nullptr;
InputSync *input_sync = nullptr;
bool first_frame_sent = false;
uint64_t heartbeat = 0;

} // namespace

void tg_renderer_lockdown(const String &p_pack_path) {
	print_line("[RENDERER-START]");

	Ref<Sandbox> sandbox = Sandbox::create();
	if (sandbox.is_valid() && sandbox->is_target()) {
		if (sandbox->lower_token() != OK) {
			CRASH_NOW_MSG("Sandbox::lower_token failed; renderer aborting (sandbox lockdown is required).");
		}
	}

	{
		SandboxDiagnostics diag(p_pack_path);
		print_line(diag.to_json_block());
	}

	print_line("[RENDERER-LOCKED]");
}

bool tg_renderer_boot(DisplayServer *p_display_server) {
	command_sync = memnew(CommandSync);
	command_sync->bind_commands();
	command_sync->socket_connect();

	Array arg;
	arg.append(RenderingDevice::get_singleton()->screen_get_format());
	command_sync->send_command("ext_texture_format", arg);

	arg.clear();
	const String filehandle_addr = tg_resolve_ipc_address(FILEHANDLE_PATH);
#ifdef WINDOWS_ENABLED
	arg.append(filehandle_addr + "|" + itos(OS::get_singleton()->get_process_id()));
#else
	arg.append(filehandle_addr);
#endif
	command_sync->send_command("send_filehandle", arg);

	print_line("TGExternalTexture: waiting for filehandle");
	ext_texture = memnew(TGExternalTexture);
	if (!ext_texture->recv_filehandle(FILEHANDLE_PATH)) {
		return false;
	}

	RenderingDevice::TextureView view;
	RenderingDevice::TextureFormat format;
	const Size2i size = p_display_server->window_get_size(DisplayServer::MAIN_WINDOW_ID);
	format.format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
	format.usage_bits = RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	format.width = static_cast<uint32_t>(size.width);
	format.height = static_cast<uint32_t>(size.height);
	format.depth = 1;

	if (ext_texture->import(format, view) != OK) {
		return false;
	}

	input_sync = memnew(InputSync);
	input_sync->socket_connect();

	print_line("[RENDERER-READY]");
	return true;
}

void tg_renderer_loop_iterate(uint64_t p_ticks_elapsed) {
	if (command_sync == nullptr || ext_texture == nullptr || input_sync == nullptr) {
		return;
	}

	if (!first_frame_sent && Engine::get_singleton()->get_frames_drawn() > 2) {
		command_sync->send_command("first_frame", Array());
		first_frame_sent = true;
	}

	heartbeat += p_ticks_elapsed;
	if (heartbeat > 1000000 && first_frame_sent) {
		command_sync->send_command("heartbeat", Array());
		heartbeat %= 1000000;
	}

	ext_texture->copy_from_screen();
	input_sync->receive_input_events();

	command_sync->poll_monitor();
	if (!command_sync->is_peer_connected()) {
		CRASH_NOW_MSG("CommandSync peer disconnected. Exiting child.");
	}
}
