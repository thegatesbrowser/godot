/**************************************************************************/
/*  socket_acl.h                                                          */
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

// Stamps `p_path` (file or directory) with our baseline security descriptor:
//
//   DACL: Everyone GENERIC_ALL + All Application Packages GENERIC_ALL.
//   SACL: SYSTEM_MANDATORY_LABEL_ACE NO_WRITE_UP at UNTRUSTED (S-1-16-0).
//
//   Both ACEs carry OBJECT_INHERIT + CONTAINER_INHERIT so children inherit
//   them. On a directory the function walks the tree once on the first call
//   (skipped on subsequent calls when the label is already present).
//
// Used by launcher-side IPC bind sites (`command_sync.cpp`, `input_sync.cpp`)
// so the renderer's AppContainer process can connect. The cached spawn-time
// variant used by SandboxWin lives in `windows/win_acl.h` as
// `WinACL::stamp_untrusted_label`.
//
// `p_path` accepts a raw filesystem path or a zmq-style "ipc://<path>" string;
// the "ipc://" prefix is stripped. No-op on non-Windows.
void tg_apply_untrusted_acl(const String &p_path);
