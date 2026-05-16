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

#include "signature_verify.h"

#include "core/string/print_string.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <string>

namespace {

std::wstring to_wide(const String &p_string) {
	CharString utf8 = p_string.utf8();
	int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.get_data(), -1, nullptr, 0);
	if (wlen <= 0) {
		return {};
	}
	std::wstring out;
	out.resize(static_cast<size_t>(wlen - 1));
	if (wlen > 1) {
		::MultiByteToWideChar(CP_UTF8, 0, utf8.get_data(), -1, out.data(), wlen);
	}
	return out;
}

Error verify_authenticode_chain(const wchar_t *p_path_w) {
	WINTRUST_FILE_INFO file_info = {};
	file_info.cbStruct = sizeof(file_info);
	file_info.pcwszFilePath = p_path_w;

	WINTRUST_DATA data = {};
	data.cbStruct = sizeof(data);
	data.dwUIChoice = WTD_UI_NONE;
	data.fdwRevocationChecks = WTD_REVOKE_NONE;
	data.dwUnionChoice = WTD_CHOICE_FILE;
	data.pFile = &file_info;
	data.dwStateAction = WTD_STATEACTION_VERIFY;

	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	LONG result = ::WinVerifyTrust(NULL, &action, &data);

	data.dwStateAction = WTD_STATEACTION_CLOSE;
	::WinVerifyTrust(NULL, &action, &data);

	if (result != ERROR_SUCCESS) {
		ERR_PRINT(vformat("signature_verify: WinVerifyTrust failed (0x%x)", (uint32_t)result));
		return ERR_UNAUTHORIZED;
	}
	return OK;
}

Error fingerprint_signer_sha1(const wchar_t *p_path_w, String &r_thumbprint) {
	HCERTSTORE cert_store = NULL;
	HCRYPTMSG cert_msg = NULL;
	DWORD encoding = 0;
	DWORD content_type = 0;
	DWORD format_type = 0;

	if (!::CryptQueryObject(CERT_QUERY_OBJECT_FILE, p_path_w,
				CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
				CERT_QUERY_FORMAT_FLAG_BINARY, 0,
				&encoding, &content_type, &format_type,
				&cert_store, &cert_msg, NULL)) {
		ERR_PRINT(vformat("signature_verify: CryptQueryObject failed (win=%d)", (int)::GetLastError()));
		return ERR_UNAUTHORIZED;
	}

	DWORD signer_size = 0;
	if (!::CryptMsgGetParam(cert_msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &signer_size)) {
		::CertCloseStore(cert_store, 0);
		::CryptMsgClose(cert_msg);
		ERR_PRINT("signature_verify: CryptMsgGetParam size failed");
		return ERR_UNAUTHORIZED;
	}

	Vector<uint8_t> signer_buf;
	signer_buf.resize(signer_size);
	if (!::CryptMsgGetParam(cert_msg, CMSG_SIGNER_INFO_PARAM, 0, signer_buf.ptrw(), &signer_size)) {
		::CertCloseStore(cert_store, 0);
		::CryptMsgClose(cert_msg);
		ERR_PRINT("signature_verify: CryptMsgGetParam read failed");
		return ERR_UNAUTHORIZED;
	}

	CMSG_SIGNER_INFO *signer = reinterpret_cast<CMSG_SIGNER_INFO *>(signer_buf.ptrw());

	CERT_INFO cert_info = {};
	cert_info.Issuer = signer->Issuer;
	cert_info.SerialNumber = signer->SerialNumber;
	PCCERT_CONTEXT cert_ctx = ::CertFindCertificateInStore(cert_store,
			X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
			0, CERT_FIND_SUBJECT_CERT, &cert_info, NULL);

	if (!cert_ctx) {
		::CertCloseStore(cert_store, 0);
		::CryptMsgClose(cert_msg);
		ERR_PRINT("signature_verify: signer cert not found in store");
		return ERR_UNAUTHORIZED;
	}

	BYTE thumbprint[20] = {};
	DWORD thumb_size = sizeof(thumbprint);
	if (!::CertGetCertificateContextProperty(cert_ctx, CERT_HASH_PROP_ID,
				thumbprint, &thumb_size)) {
		::CertFreeCertificateContext(cert_ctx);
		::CertCloseStore(cert_store, 0);
		::CryptMsgClose(cert_msg);
		ERR_PRINT("signature_verify: CertGetCertificateContextProperty failed");
		return ERR_UNAUTHORIZED;
	}

	String observed;
	for (size_t i = 0; i < sizeof(thumbprint); ++i) {
		observed += vformat("%02X", thumbprint[i]);
	}
	r_thumbprint = observed;

	::CertFreeCertificateContext(cert_ctx);
	::CertCloseStore(cert_store, 0);
	::CryptMsgClose(cert_msg);
	return OK;
}

} // namespace

Error tg_verify_renderer_binary(const String &p_path, const String &p_expected_thumbprint) {
	const std::wstring path_w = to_wide(p_path);

	Error err = verify_authenticode_chain(path_w.c_str());
	if (err != OK) {
		return err;
	}

	String observed;
	err = fingerprint_signer_sha1(path_w.c_str(), observed);
	if (err != OK) {
		return err;
	}

	const String expected = p_expected_thumbprint.to_upper().replace(":", "").replace(" ", "");
	if (observed != expected) {
		ERR_PRINT(vformat("signature_verify: thumbprint mismatch (expected %s, got %s)", expected, observed));
		return ERR_UNAUTHORIZED;
	}

	return OK;
}
