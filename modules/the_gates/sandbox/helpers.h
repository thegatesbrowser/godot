#ifndef HELPERS_H
#define HELPERS_H

#include <wchar.h>

wchar_t* to_wchar(const char* cstr) {
    if (!cstr) {
        return nullptr;
    }

    size_t wcLength = 0;
    errno_t err = mbstowcs_s(&wcLength, nullptr, 0, cstr, 0);
    if (err != 0 || wcLength == 0) {
        return nullptr;
    }

    wchar_t* wstring = new wchar_t[wcLength];
    err = mbstowcs_s(&wcLength, wstring, wcLength, cstr, _TRUNCATE);
    if (err != 0) {
        delete[] wstring;
        return nullptr;
    }

    return wstring;
}

wchar_t* to_wchar(const String& str) {
    return to_wchar(str.utf8().get_data());
}

#endif // HELPERS_H
