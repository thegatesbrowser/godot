/**************************************************************************/
/*  socket_acl_win.h                                                      */
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

#ifndef SOCKET_ACL_WIN_H
#define SOCKET_ACL_WIN_H

#include "core/string/ustring.h"

// On Windows, stamps the AF_UNIX socket file at `p_zmq_address` with a
// security descriptor permissive enough for a Chromium-sandboxed renderer
// (USER_LIMITED token, UNTRUSTED integrity level) to connect() into it.
//
// What it sets:
//   DACL: GENERIC_ALL grant to "Everyone" (S-1-1-0) — the USER_LIMITED
//         token keeps Everyone as an enabled SID, so this is what the
//         renderer's effective access check sees.
//   SACL: SYSTEM_MANDATORY_LABEL_ACE with NO_WRITE_UP at integrity SID
//         S-1-16-0 (UNTRUSTED). Default file integrity is Medium, which
//         Windows MIC would otherwise block UNTRUSTED writes against.
//
// `p_zmq_address` accepts either a raw filesystem path or a zmq-style
// "ipc://<path>" string — the "ipc://" prefix is stripped before the call
// to SetNamedSecurityInfo.
//
// No-op on non-Windows platforms. Logs (but doesn't fail) on any Windows
// error so a missing socket file or permissions issue doesn't kill the
// whole IPC setup.
void tg_apply_socket_acl_for_sandbox(const String &p_zmq_address);

#endif // SOCKET_ACL_WIN_H
