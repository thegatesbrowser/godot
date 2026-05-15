/**************************************************************************/
/*  socket_acl_win.cpp                                                    */
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

#include "socket_acl_win.h"

#ifdef WINDOWS_ENABLED

#include "core/string/print_string.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

// SDDL:
//   D:(A;;GA;;;WD)         DACL: allow GENERIC_ALL to Everyone (WD).
//   S:(ML;;NW;;;LW)        SACL: mandatory label, NO_WRITE_UP at LW
//                            (Low). Note: there is no SDDL alias for the
//                            UNTRUSTED level — "LW" maps to Low (S-1-16-4096)
//                            and any process at IL >= Low (which includes
//                            UNTRUSTED S-1-16-0 under NO_WRITE_UP semantics
//                            inverted: NW means "no write UP from below this
//                            label", so a Low label allows writes from
//                            anything at Low or higher; UNTRUSTED is BELOW
//                            Low). We want UNTRUSTED to be able to write, so
//                            we need the label AT UNTRUSTED. Use the raw SID
//                            S-1-16-0 directly.
static const wchar_t *kSocketSddl =
		L"D:(A;;GA;;;WD)S:(ML;;NW;;;S-1-16-0)";

void tg_apply_socket_acl_for_sandbox(const String &p_zmq_address) {
	// Strip the zmq transport prefix.
	String path = p_zmq_address;
	const String prefix = "ipc://";
	if (path.begins_with(prefix)) {
		path = path.substr(prefix.length());
	}
	if (path.is_empty()) {
		return;
	}

	PSECURITY_DESCRIPTOR psd = nullptr;
	if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
				kSocketSddl, SDDL_REVISION_1, &psd, nullptr)) {
		print_line(vformat("tg_apply_socket_acl_for_sandbox: "
						   "ConvertStringSecurityDescriptor failed (win=%d)",
				(int)::GetLastError()));
		return;
	}

	BOOL dacl_present = FALSE, dacl_defaulted = FALSE;
	BOOL sacl_present = FALSE, sacl_defaulted = FALSE;
	PACL pdacl = nullptr;
	PACL psacl = nullptr;
	::GetSecurityDescriptorDacl(psd, &dacl_present, &pdacl, &dacl_defaulted);
	::GetSecurityDescriptorSacl(psd, &sacl_present, &psacl, &sacl_defaulted);

	const Char16String path_utf16 = path.utf16();
	const DWORD r = ::SetNamedSecurityInfoW(
			(LPWSTR)path_utf16.get_data(),
			SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
			/*owner=*/nullptr, /*group=*/nullptr,
			pdacl, psacl);

	::LocalFree(psd);

	if (r != ERROR_SUCCESS) {
		print_line(vformat("tg_apply_socket_acl_for_sandbox: "
						   "SetNamedSecurityInfo(%s) failed (win=%d)",
				path, (int)r));
	}
}

#else // !WINDOWS_ENABLED

void tg_apply_socket_acl_for_sandbox(const String & /*p_zmq_address*/) {
	// No-op: MIC and AF_UNIX-via-afunix.sys are Windows specifics. On
	// Linux / macOS the AF_UNIX socket file permissions inherit from the
	// process umask and the parent dir, which is enough — the launcher's
	// user_data_dir is already in the user's home tree.
}

#endif // WINDOWS_ENABLED
