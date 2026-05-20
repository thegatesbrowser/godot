/**************************************************************************/
/*  signature_verify.cpp                                                  */
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

#ifdef LINUXBSD_ENABLED

#include "signature_verify.h"

#include "sha256_file.h"

#include "core/string/print_string.h"

namespace {

String to_hex(const uint8_t p_digest[32]) {
	String hex;
	for (int i = 0; i < 32; ++i) {
		hex += String::num_int64(p_digest[i] >> 4, 16, false);
		hex += String::num_int64(p_digest[i] & 0x0F, 16, false);
	}
	return hex;
}

} // namespace

Error tg_verify_renderer_binary(const String &p_path, const String &p_pin_hex) {
	uint8_t digest[32] = { 0 };
	const Error err = tg_sha256_file(p_path, digest);
	if (err != OK) {
		return err;
	}

	const String observed = to_hex(digest);

	if (p_pin_hex.is_empty()) {
		print_line(vformat("[VERIFY-BYPASSED] SandboxLinux: no tg_signature_pin set at build time; observed=%s", observed));
		return OK;
	}

	// Tolerate copy-paste from sha256sum (whitespace) or openssl-style colons.
	const String expected = p_pin_hex.strip_edges().replace(":", "").replace(" ", "").to_lower();
	if (observed != expected) {
		ERR_FAIL_V_MSG(ERR_UNAUTHORIZED,
				vformat("SandboxLinux::verify_binary: signature mismatch expected=%s got=%s", expected, observed));
	}
	print_line(vformat("SandboxLinux: signature pin matched (%s)", observed));
	return OK;
}

#endif // LINUXBSD_ENABLED
