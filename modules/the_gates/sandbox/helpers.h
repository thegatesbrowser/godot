#ifndef HELPERS_H
#define HELPERS_H

#include <wchar.h>

wchar_t to_wchar(const char* cstr) {
    wchar_t wstring;
    size_t wcLength = mbstowcs(NULL, cstr, 0);
    mbstowcs(&wstring, cstr, wcLength + 1);
    return wstring;
}

wchar_t *to_wchar_line(int argc, const wchar_t* argv[]) {
    int totalLength = 0;
    for (int i = 1; i < argc; ++i) {
        totalLength += wcslen(argv[i]);
    }

    // Allocate memory for the concatenated string
    wchar_t* concatenated = new wchar_t[totalLength + 1];
    concatenated[0] = '\0'; // Ensure the string is initially empty

    // Concatenate the strings
    for (int i = 1; i < argc; ++i) {
        wcscat(concatenated, argv[i]);
    }

    return concatenated;
}

#endif // HELPERS_H
