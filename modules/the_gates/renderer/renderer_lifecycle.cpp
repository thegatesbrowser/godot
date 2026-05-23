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
#include "../network/brokered_net_socket.h"
#include "../network/renderer_net_client.h"
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

constexpr uint64_t HEARTBEAT_INTERVAL_USEC = 1'000'000;

CommandSync *command_sync = nullptr;
TGExternalTexture *ext_texture = nullptr;
InputSync *input_sync = nullptr;
bool first_frame_sent = false;
uint64_t heartbeat = 0;

// Captured on first call; shared by tg_renderer_phase and the [ITER] log.
uint64_t epoch_ms() {
	static const uint64_t t0 = OS::get_singleton()->get_ticks_msec();
	return t0;
}

void exchange_filehandle() {
	const String filehandle_addr = tg_resolve_ipc_address(FILEHANDLE_PATH);
	Array arg;
#ifdef WINDOWS_ENABLED
	arg.append(filehandle_addr + "|" + itos(OS::get_singleton()->get_process_id()));
#else
	arg.append(filehandle_addr);
#endif
	command_sync->send_command("send_filehandle", arg);

	print_line("TGExternalTexture: waiting for filehandle");
	ext_texture = memnew(TGExternalTexture);
	if (!ext_texture->recv_filehandle(FILEHANDLE_PATH)) {
		CRASH_NOW_MSG("recv_filehandle failed");
	}
}

bool import_external_texture(DisplayServer *p_display_server) {
	Array arg;
	arg.append(RenderingDevice::get_singleton()->screen_get_format());
	command_sync->send_command("ext_texture_format", arg);

	RenderingDevice::TextureView view;
	RenderingDevice::TextureFormat format;
	const Size2i size = p_display_server->window_get_size(DisplayServer::MAIN_WINDOW_ID);
	format.format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
	format.usage_bits = RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	format.width = static_cast<uint32_t>(size.width);
	format.height = static_cast<uint32_t>(size.height);
	format.depth = 1;

	return ext_texture->import(format, view) == OK;
}

// Returns the integer value of --tg-broker-fd=<n>, or -1 if the flag is
// missing or malformed. Reports the failure mode in r_error for the caller.
intptr_t parse_broker_fd_from_argv(String &r_error) {
	const List<String> args = OS::get_singleton()->get_cmdline_args();
	const String prefix = "--tg-broker-fd=";
	for (const String &arg : args) {
		if (!arg.begins_with(prefix)) {
			continue;
		}
		const String value = arg.substr(prefix.length());
		if (!value.is_valid_int()) {
			r_error = "--tg-broker-fd has non-integer value";
			return -1;
		}
		return (intptr_t)value.to_int();
	}
	r_error = "--tg-broker-fd missing from argv";
	return -1;
}

// Adopts the broker control FD/HANDLE inherited from the launcher:
//   POSIX: socketpair half dup'd via posix_spawn_file_actions
//   Windows: named-pipe HANDLE inherited via Chromium TargetPolicy::AddHandleToShare
// Numeric value arrives as --tg-broker-fd=<n> on argv. Installs BrokeredNetSocket
// as the engine NetSocket factory and the DNS hook — both pull from
// RendererNetClient. Must run pre-lockdown; afterwards no new FDs can be created.
void engage_network_broker() {
	String error;
	const intptr_t broker_handle = parse_broker_fd_from_argv(error);
	if (broker_handle < 0) {
		CRASH_NOW_MSG(vformat("Renderer cannot establish network broker: %s (launcher contract violated).", error));
	}
	if (RendererNetClient::install(broker_handle) != OK) {
		CRASH_NOW_MSG("RendererNetClient::install failed; refusing to lower token without network broker.");
	}
	BrokeredNetSocket::make_default();
	print_line("[NETWORK-BROKER] inherited handle=" + itos((int64_t)broker_handle) +
			"; renderer NetSocket + IP factories installed.");
}

void lockdown(const String &p_pack_path) {
	Ref<Sandbox> sandbox = Sandbox::create();
	if (sandbox.is_valid() && sandbox->is_target()) {
		// Survives __debugbreak; the negative-fail-closed harness checks for this marker.
		print_line("[LOCKDOWN-ATTEMPT]");
		if (sandbox->lower_token() != OK) {
			CRASH_NOW_MSG("Sandbox::lower_token failed; renderer aborting (sandbox lockdown is required).");
		}
	}

	SandboxDiagnostics diag(p_pack_path);
	print_line(diag.to_json_block());

	print_line("[RENDERER-LOCKED]");
	tg_renderer_phase("lockdown_done");
}

} // namespace

void tg_renderer_phase(const char *p_label) {
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	const uint64_t t0 = epoch_ms();
	static uint64_t t_prev = t0;
	const uint64_t since_start = now - t0;
	const uint64_t since_prev = now - t_prev;
	print_line(vformat("[PHASE] %s  t=%dms  delta=%dms", String::utf8(p_label), (int64_t)since_start, (int64_t)since_prev));
	t_prev = now;
}

bool tg_renderer_engage(DisplayServer *p_display_server, const String &p_pack_path) {
	print_line("[RENDERER-START]");

	command_sync = memnew(CommandSync);
	command_sync->bind_commands();
	command_sync->socket_connect();

	exchange_filehandle();

	input_sync = memnew(InputSync);
	input_sync->socket_connect();

	if (!import_external_texture(p_display_server)) {
		return false;
	}

	// MUST happen before lockdown: opens AF_UNIX socket to the broker and
	// installs NetSocket/IP factory overrides. Post-lockdown, no new
	// sockets can be created — only the broker FD remains usable.
	engage_network_broker();

	lockdown(p_pack_path);
	return true;
}

void tg_renderer_boot() {
	print_line("[RENDERER-READY]");
}

void tg_renderer_loop_iterate(uint64_t p_ticks_elapsed) {
	if (command_sync == nullptr || ext_texture == nullptr || input_sync == nullptr) {
		return;
	}

	if (!first_frame_sent) {
		static int iter = 0;
		const uint32_t drawn = Engine::get_singleton()->get_frames_drawn();
		print_line(vformat("[ITER] %d frames_drawn=%d t=%dms",
				iter++, (int)drawn,
				(int)(OS::get_singleton()->get_ticks_msec() - epoch_ms())));
		if (drawn > 2) {
			command_sync->send_command("first_frame", Array());
			first_frame_sent = true;
		}
	}

	heartbeat += p_ticks_elapsed;
	if (heartbeat > HEARTBEAT_INTERVAL_USEC && first_frame_sent) {
		command_sync->send_command("heartbeat", Array());
		heartbeat %= HEARTBEAT_INTERVAL_USEC;
	}

	ext_texture->copy_from_screen();
	input_sync->receive_input_events();

	command_sync->poll_monitor();
	if (!command_sync->is_peer_connected()) {
		CRASH_NOW_MSG("CommandSync peer disconnected. Exiting child.");
	}
}
