/**************************************************************************/
/*  renderer_lifecycle.h                                                  */
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

#pragma once

#include "core/string/ustring.h"

class DisplayServer;

// Renderer-process orchestration moved out of main.cpp. main.cpp's
// #ifdef TG_RENDERER blocks shrink to one-line calls into these functions
// — the engine touchpoint stays minimal (good for fork merge ergonomics).
//
// tg_renderer_boot: brings up CommandSync + InputSync + the shared Vulkan
// texture, then calls Sandbox::lower_token (fail-closed) and dumps the
// SandboxDiagnostics block. Returns false to abort engine startup, true on
// success.
//
// tg_renderer_loop_iterate: per-frame first-frame signal, heartbeat,
// copy_from_screen, receive_input_events, CRASH_NOW on CommandSync peer
// disconnect.
bool tg_renderer_boot(DisplayServer *p_display_server, const String &p_pack_path);
void tg_renderer_loop_iterate(uint64_t p_ticks_elapsed);
