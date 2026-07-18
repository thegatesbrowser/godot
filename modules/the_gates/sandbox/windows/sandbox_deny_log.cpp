/**************************************************************************/
/*  sandbox_deny_log.cpp                                                  */
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

#include "sandbox_deny_log.h"

#include "../signal_safe_log.h"

namespace {

constexpr uint32_t DENIAL_TABLE_SIZE = 256;
constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;
constexpr uint8_t DENIAL_KIND_FILE = 1;
constexpr uint8_t DENIAL_KIND_REGISTRY = 2;
constexpr int MAX_NAME_WIDE_CHARS = 200;
constexpr int LOG_BUFFER_SIZE = 768;

uint64_t g_denial_seen[DENIAL_TABLE_SIZE] = {};

uint64_t hash_denial(uint8_t p_kind, const wchar_t *p_name) {
	uint64_t hash = FNV_OFFSET_BASIS;
	hash ^= p_kind;
	hash *= FNV_PRIME;

	if (p_name) {
		while (*p_name) {
			const uint16_t character = (uint16_t)*p_name++;
			hash ^= (uint8_t)(character & 0xff);
			hash *= FNV_PRIME;
			hash ^= (uint8_t)(character >> 8);
			hash *= FNV_PRIME;
		}
	}

	return hash == 0 ? 1 : hash;
}

bool mark_denial_seen(uint64_t p_hash) {
	const uint32_t start = (uint32_t)(p_hash ^ (p_hash >> 32));
	for (uint32_t i = 0; i < DENIAL_TABLE_SIZE; ++i) {
		const uint32_t slot = (start + i) & (DENIAL_TABLE_SIZE - 1);
		if (g_denial_seen[slot] == p_hash) {
			return false;
		}
		if (g_denial_seen[slot] == 0) {
			g_denial_seen[slot] = p_hash;
			return true;
		}
	}
	return false;
}

void append_text(char *p_buffer, int p_capacity, int &r_length, const char *p_text) {
	while (*p_text && r_length < p_capacity) {
		p_buffer[r_length++] = *p_text++;
	}
}

void append_hex_32(char *p_buffer, int p_capacity, int &r_length, uint32_t p_value) {
	const char hex_digits[] = "0123456789abcdef";
	for (int shift = 28; shift >= 0 && r_length < p_capacity; shift -= 4) {
		p_buffer[r_length++] = hex_digits[(p_value >> shift) & 0xf];
	}
}

void append_wide_name(char *p_buffer, int p_capacity, int &r_length, const wchar_t *p_name) {
	if (!p_name) {
		append_text(p_buffer, p_capacity, r_length, "<null>");
		return;
	}

	int wide_length = 0;
	while (p_name[wide_length] && wide_length < MAX_NAME_WIDE_CHARS) {
		++wide_length;
	}
	const int available = p_capacity - r_length;
	if (available <= 0) {
		return;
	}
	const int converted = WideCharToMultiByte(CP_UTF8, 0, p_name, wide_length,
			p_buffer + r_length, available, nullptr, nullptr);
	if (converted > 0) {
		r_length += converted;
	}
}

void log_denied(uint8_t p_kind, const char *p_operation, const wchar_t *p_name, uint32_t p_access) {
	if (!mark_denial_seen(hash_denial(p_kind, p_name))) {
		return;
	}

	char buffer[LOG_BUFFER_SIZE];
	int length = 0;
	append_text(buffer, LOG_BUFFER_SIZE - 1, length, "[SANDBOX-WIN-DENY] ");
	append_text(buffer, LOG_BUFFER_SIZE - 1, length, p_operation);
	append_text(buffer, LOG_BUFFER_SIZE - 1, length, " access=0x");
	append_hex_32(buffer, LOG_BUFFER_SIZE - 1, length, p_access);
	append_text(buffer, LOG_BUFFER_SIZE - 1, length, " name=");
	append_wide_name(buffer, LOG_BUFFER_SIZE - 1, length, p_name);
	buffer[length++] = '\n';
	tg_signal_safe_emit(buffer, length);
}

} // namespace

void tg_sandbox_log_denied_file(const wchar_t *p_name, uint32_t p_access) {
	log_denied(DENIAL_KIND_FILE, "file", p_name, p_access);
}

void tg_sandbox_log_denied_registry(const wchar_t *p_name, uint32_t p_access) {
	log_denied(DENIAL_KIND_REGISTRY, "registry", p_name, p_access);
}
