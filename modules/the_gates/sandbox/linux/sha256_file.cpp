/**************************************************************************/
/*  sha256_file.cpp                                                       */
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

#include "sha256_file.h"

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/templates/vector.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__x86_64__)
extern "C" void sha256_block_data_order_hw(uint32_t state[8], const uint8_t *data, size_t num_blocks);
#endif

namespace {

constexpr uint32_t kSha256InitState[8] = {
	0x6a09e667,
	0xbb67ae85,
	0x3c6ef372,
	0xa54ff53a,
	0x510e527f,
	0x9b05688c,
	0x1f83d9ab,
	0x5be0cd19,
};

bool sha_ni_available() {
#if defined(__x86_64__)
	return __builtin_cpu_supports("sha");
#else
	return false;
#endif
}

void finalize_state(const uint32_t p_state[8], uint8_t r_digest[32]) {
	for (int i = 0; i < 8; ++i) {
		r_digest[i * 4 + 0] = (uint8_t)(p_state[i] >> 24);
		r_digest[i * 4 + 1] = (uint8_t)(p_state[i] >> 16);
		r_digest[i * 4 + 2] = (uint8_t)(p_state[i] >> 8);
		r_digest[i * 4 + 3] = (uint8_t)(p_state[i]);
	}
}

// sha256_block_data_order_hw processes whole 64-byte blocks only; FIPS 180-4
// §5.1.1 requires appending 0x80, zero-padding to (len mod 64 == 56), then
// the message bit-length as a 64-bit big-endian word. Synthesizing the tail
// in a 128-byte stack buffer lets the bulk stay in the mmap region.
size_t build_padded_tail(uint64_t p_total_bytes, const uint8_t *p_tail, size_t p_tail_len,
		uint8_t r_padded[128]) {
	memcpy(r_padded, p_tail, p_tail_len);
	r_padded[p_tail_len] = 0x80;
	const size_t total = (p_tail_len >= 56) ? 128 : 64;
	memset(r_padded + p_tail_len + 1, 0, total - p_tail_len - 1 - 8);
	const uint64_t bits = p_total_bytes * 8;
	for (int i = 0; i < 8; ++i) {
		r_padded[total - 8 + i] = (uint8_t)(bits >> (56 - i * 8));
	}
	return total;
}

#if defined(__x86_64__)
Error hash_via_shaext(const String &p_path, uint8_t r_digest[32]) {
	const CharString path_cs = p_path.utf8();
	const int fd = ::open(path_cs.get_data(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		ERR_FAIL_V_MSG(ERR_UNAUTHORIZED,
				vformat("tg_sha256_file: open %s failed errno=%d", p_path, (int)errno));
	}

	struct stat st = {};
	if (::fstat(fd, &st) != 0) {
		const int saved = errno;
		::close(fd);
		ERR_FAIL_V_MSG(ERR_UNAUTHORIZED,
				vformat("tg_sha256_file: fstat %s failed errno=%d", p_path, saved));
	}
	const size_t size = (size_t)st.st_size;

	const void *map = (size > 0)
			? ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0)
			: nullptr;
	::close(fd);
	if (size > 0 && map == MAP_FAILED) {
		ERR_FAIL_V_MSG(ERR_UNAUTHORIZED,
				vformat("tg_sha256_file: mmap %s failed errno=%d", p_path, (int)errno));
	}

	uint32_t state[8];
	memcpy(state, kSha256InitState, sizeof(state));

	const uint8_t *data = static_cast<const uint8_t *>(map);
	const size_t whole_blocks = size >> 6;
	if (whole_blocks > 0) {
		sha256_block_data_order_hw(state, data, whole_blocks);
	}

	uint8_t padded[128];
	const size_t tail_off = whole_blocks << 6;
	const size_t padded_len = build_padded_tail((uint64_t)size,
			data + tail_off, size - tail_off, padded);
	sha256_block_data_order_hw(state, padded, padded_len >> 6);

	if (size > 0) {
		::munmap(const_cast<void *>(map), size);
	}

	finalize_state(state, r_digest);
	return OK;
}
#endif

Error hash_via_mbedtls(const String &p_path, uint8_t r_digest[32]) {
	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &err);
	if (file.is_null()) {
		ERR_FAIL_V_MSG(ERR_UNAUTHORIZED,
				vformat("tg_sha256_file: cannot open %s err=%d", p_path, (int)err));
	}

	CryptoCore::SHA256Context ctx;
	ctx.start();

	constexpr int CHUNK = 64 * 1024;
	Vector<uint8_t> buffer;
	buffer.resize(CHUNK);
	while (!file->eof_reached()) {
		const int64_t n = file->get_buffer(buffer.ptrw(), CHUNK);
		if (n > 0) {
			ctx.update(buffer.ptr(), (size_t)n);
		}
		if (n < CHUNK) {
			break;
		}
	}
	ctx.finish(r_digest);
	return OK;
}

} // namespace

Error tg_sha256_file(const String &p_path, uint8_t r_digest[32]) {
#if defined(__x86_64__)
	if (sha_ni_available()) {
		return hash_via_shaext(p_path, r_digest);
	}
#endif
	return hash_via_mbedtls(p_path, r_digest);
}

#endif // LINUXBSD_ENABLED
