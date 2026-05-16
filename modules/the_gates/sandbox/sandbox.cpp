/**************************************************************************/
/*  sandbox.cpp                                                           */
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

#include "sandbox.h"

#include "sandbox_policy.h"

#if defined(TG_SANDBOX) && defined(WINDOWS_ENABLED)
#include "windows/sandbox_win.h"
#endif

Ref<Sandbox> Sandbox::create() {
#if defined(TG_SANDBOX) && defined(WINDOWS_ENABLED)
	return Ref<Sandbox>(memnew(SandboxWin));
#else
	return Ref<Sandbox>();
#endif
}

void Sandbox::_bind_methods() {
	ClassDB::bind_static_method("Sandbox", D_METHOD("create"), &Sandbox::create);
	ClassDB::bind_method(D_METHOD("spawn_target", "policy", "executable", "arguments"),
			&Sandbox::spawn_target);
	ClassDB::bind_method(D_METHOD("apply_renderer_acl", "path"), &Sandbox::apply_renderer_acl);
	ClassDB::bind_method(D_METHOD("verify_binary", "path"), &Sandbox::verify_binary);
	ClassDB::bind_method(D_METHOD("is_target_running"), &Sandbox::is_target_running);
	ClassDB::bind_method(D_METHOD("kill_target"), &Sandbox::kill_target);
	ClassDB::bind_method(D_METHOD("lower_token"), &Sandbox::lower_token);
	ClassDB::bind_method(D_METHOD("is_target"), &Sandbox::is_target);
}
