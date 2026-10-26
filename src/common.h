#pragma once

// foo_mouse_gesture_host - общие заголовки компонента.
//
// Всё, что нужно компоненту, берётся из официального foobar2000 SDK
// (целевая версия API - foobar2000 v2.0, x64/x86). Мы намеренно не тянем
// ATL/WTL: интерфейс настроек целиком построен на "чистом" Win32 API.

#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/SDK/coreDarkMode.h>

#include <pfc/string-conv-lite.h>

#include "logic_common.h"

namespace mgh {

//! Перевод строки UTF-16 -> UTF-8.
inline std::string utf8_from_wide(const wchar_t* text) {
    if (text == nullptr) return {};
    const pfc::string8 converted = pfc::utf8FromWide(text);
    return std::string(converted.c_str(), converted.length());
}

//! Перевод строки UTF-8 -> UTF-16.
inline std::wstring wide_from_utf8(std::string_view text) {
    const pfc::wstringLite converted = pfc::wideFromUTF8(text.data(), text.size());
    return std::wstring(converted.c_str(), converted.length());
}

} // namespace mgh
