#pragma once

// Стандартная часть общих helpers: не зависит от Win32 и foobar2000 SDK.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mgh {

inline int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

inline std::string ascii_upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

inline std::string trim_ascii(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
    return std::string(text.substr(begin, end - begin));
}

//! Убирает "&" (мнемоники Win32-меню) из строки.
inline std::string strip_mnemonics(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&') {
            if (i + 1 < text.size() && text[i + 1] == '&') {
                out.push_back('&');
                ++i;
            }
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

} // namespace mgh
