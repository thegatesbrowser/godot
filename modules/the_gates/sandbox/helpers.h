#ifndef HELPERS_H
#define HELPERS_H

#include <wchar.h>

// Converts a narrow string to a wide string
wchar_t* to_wchar(const char* cstr) {
    if (!cstr) {
        return nullptr; // Handle null input
    }

    size_t wcLength = std::mbstowcs(nullptr, cstr, 0);
    if (wcLength == static_cast<size_t>(-1)) {
        return nullptr; // Conversion failed
    }

    wchar_t* wstring = new wchar_t[wcLength + 1];
    std::mbstowcs(wstring, cstr, wcLength + 1);
    return wstring;
}

#endif // HELPERS_H
