/**************************************************************************/
/*  signature_verify.mm                                                   */
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

#ifdef MACOS_ENABLED

#include "signature_verify.h"

#include "core/string/print_string.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

String to_hex(const uint8_t p_digest[CC_SHA256_DIGEST_LENGTH]) {
	String hex;
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
		hex += String::num_int64(p_digest[i] >> 4, 16, false);
		hex += String::num_int64(p_digest[i] & 0x0F, 16, false);
	}
	return hex;
}

Error sha256_file(const String &p_path, uint8_t r_digest[CC_SHA256_DIGEST_LENGTH]) {
	const CharString utf8_path = p_path.utf8();
	const int fd = ::open(utf8_path.get_data(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return ERR_FILE_CANT_OPEN;
	}
	struct stat st = {};
	if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
		::close(fd);
		return ERR_FILE_CANT_READ;
	}

	void *map = ::mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	::close(fd);
	if (map == MAP_FAILED) {
		return ERR_FILE_CANT_READ;
	}
	CC_SHA256(map, (CC_LONG)st.st_size, r_digest);
	::munmap(map, (size_t)st.st_size);
	return OK;
}

} // namespace

Error tg_verify_renderer_binary(const String &p_path, const String &p_pin_hex) {
	uint8_t digest[CC_SHA256_DIGEST_LENGTH] = { 0 };
	const Error err = sha256_file(p_path, digest);
	if (err != OK) {
		return err;
	}

	const String observed = to_hex(digest);

	if (p_pin_hex.is_empty()) {
		print_line(vformat("[VERIFY-BYPASSED] SandboxMacOS: no tg_signature_pin set at build time; observed=%s", observed));
		return OK;
	}

	const String expected = p_pin_hex.strip_edges().replace(":", "").replace(" ", "").to_lower();
	if (observed != expected) {
		ERR_FAIL_V_MSG(ERR_UNAUTHORIZED,
				vformat("SandboxMacOS::verify_binary: signature mismatch expected=%s got=%s", expected, observed));
	}
	print_line(vformat("SandboxMacOS: signature pin matched (%s)", observed));
	return OK;
}

#endif // MACOS_ENABLED
