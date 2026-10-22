#include "gesture_tokens.h"

namespace mgh {
namespace {

struct TokenInfo {
    Token token;
    const char* id;
    const char* symbol;
};

// Стрелки записаны UTF-8 escape-последовательностями, чтобы файл
// оставался корректным при любой кодировке компиляции.
const TokenInfo kTokenTable[] = {
    { Token::up,                 "U",   "\xE2\x86\x91" },         // ↑
    { Token::down,               "D",   "\xE2\x86\x93" },         // ↓
    { Token::left,               "L",   "\xE2\x86\x90" },         // ←
    { Token::right,              "R",   "\xE2\x86\x92" },         // →
    { Token::up_left,            "UL",  "\xE2\x86\x96" },         // ↖
    { Token::up_right,           "UR",  "\xE2\x86\x97" },         // ↗
    { Token::down_left,          "DL",  "\xE2\x86\x99" },         // ↙
    { Token::down_right,         "DR",  "\xE2\x86\x98" },         // ↘
    { Token::wheel_up,           "WU",  "W\xE2\x86\x91" },        // W↑
    { Token::wheel_down,         "WD",  "W\xE2\x86\x93" },        // W↓
    { Token::rocker_right_left,  "R+L", "R+L" },
    { Token::rocker_left_right,  "L+R", "L+R" },
};

const TokenInfo* find_info(Token token) {
    for (const auto& info : kTokenTable) {
        if (info.token == token) return &info;
    }
    return nullptr;
}

const TokenInfo* find_info(std::string_view id) {
    for (const auto& info : kTokenTable) {
        if (iequals_ascii(info.id, id)) return &info;
        // Пользователь может ввести жест и стрелками (то, что видно в поле
        // "Gesture"), поэтому принимаем и символьное представление.
        if (id == info.symbol) return &info;
    }
    return nullptr;
}

bool token_is_cardinal(Token token) {
    return token == Token::up || token == Token::down ||
           token == Token::left || token == Token::right;
}

//! Две основные составляющие диагонали. Порядок соответствует обходу
//! D -> R -> U -> L -> D в экранных координатах.
bool diagonal_components(Token token, Token& first, Token& second) {
    switch (token) {
    case Token::down_right: first = Token::down;  second = Token::right; return true;
    case Token::up_right:   first = Token::right; second = Token::up;    return true;
    case Token::up_left:    first = Token::up;    second = Token::left;  return true;
    case Token::down_left:  first = Token::left;  second = Token::down;  return true;
    default: return false;
    }
}

void append_token(std::vector<Token>& tokens, Token token) {
    if (tokens.empty() || tokens.back() != token) tokens.push_back(token);
}

} // namespace

std::string token_id(Token token) {
    const TokenInfo* info = find_info(token);
    return info ? std::string(info->id) : std::string();
}

std::string token_symbol(Token token) {
    const TokenInfo* info = find_info(token);
    return info ? std::string(info->symbol) : std::string();
}

bool token_from_id(std::string_view text, Token& out) {
    const TokenInfo* info = find_info(text);
    if (info == nullptr) return false;
    out = info->token;
    return true;
}

bool token_is_directional(Token token) {
    switch (token) {
    case Token::up:
    case Token::down:
    case Token::left:
    case Token::right:
    case Token::up_left:
    case Token::up_right:
    case Token::down_left:
    case Token::down_right:
        return true;
    default:
        return false;
    }
}

std::string join_tokens(const std::vector<Token>& tokens) {
    std::string out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) out.push_back(' ');
        out += token_id(tokens[i]);
    }
    return out;
}

std::vector<std::string> gesture_match_candidates(const std::vector<Token>& tokens) {
    std::vector<std::string> candidates;

    const auto add_candidate = [&candidates](const std::vector<Token>& value) {
        const std::string gesture = join_tokens(value);
        for (const auto& existing : candidates) {
            if (existing == gesture) return;
        }
        candidates.push_back(gesture);
    };

    // Точное совпадение всегда имеет наивысший приоритет.
    add_candidate(tokens);
    if (tokens.empty()) return candidates;

    std::size_t cardinal_count = 0;
    std::size_t diagonal_count = 0;
    for (const Token token : tokens) {
        if (!token_is_directional(token)) return candidates;
        if (token_is_cardinal(token)) ++cardinal_count;
        else ++diagonal_count;
    }

    const auto add_relaxed = [&add_candidate](std::vector<Token> relaxed) {
        add_candidate(relaxed);
        if (relaxed.size() >= 4 && relaxed.front() == relaxed.back()) {
            relaxed.pop_back();
            add_candidate(relaxed);
        }
    };

    // Раскрываем каждую переходную диагональ в две составляющие в порядке
    // обхода. Например, полностью округлый квадрат "DR UR UL DL" превращается
    // в "D R U L D", после чего отбрасывается замыкающий поворот. Одиночную
    // диагональ не раскрываем: это было бы слишком неоднозначно. Точные
    // диагональные привязки всё равно проверяются первыми.
    if (diagonal_count != 0 && tokens.size() >= 3 &&
        (cardinal_count != 0 || tokens.size() >= 4)) {
        std::vector<Token> expanded;
        for (const Token token : tokens) {
            Token first = Token::up;
            Token second = Token::up;
            if (diagonal_components(token, first, second)) {
                append_token(expanded, first);
                append_token(expanded, second);
            } else {
                append_token(expanded, token);
            }
        }
        add_relaxed(std::move(expanded));
    } else if (cardinal_count >= 4) {
        // В цепочке без диагоналей нужен только вариант без замыкающего
        // поворота к начальной точке.
        add_relaxed(tokens);
    }

    return candidates;
}

bool parse_gesture(std::string_view gesture, std::vector<Token>& out) {
    out.clear();
    std::size_t pos = 0;
    while (pos < gesture.size()) {
        while (pos < gesture.size() && (gesture[pos] == ' ' || gesture[pos] == '\t')) ++pos;
        if (pos >= gesture.size()) break;
        const std::size_t begin = pos;
        while (pos < gesture.size() && gesture[pos] != ' ' && gesture[pos] != '\t') ++pos;
        Token token{};
        if (!token_from_id(gesture.substr(begin, pos - begin), token)) {
            out.clear();
            return false;
        }
        out.push_back(token);
    }
    return true;
}

std::string normalize_gesture(std::string_view raw) {
    std::vector<Token> tokens;
    if (!parse_gesture(raw, tokens)) return std::string();
    return join_tokens(tokens);
}

std::string format_gesture(std::string_view canonical, bool alphabet) {
    std::vector<Token> tokens;
    if (!parse_gesture(canonical, tokens)) return std::string(canonical);
    std::string out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) out.push_back(' ');
        out += alphabet ? token_id(tokens[i]) : token_symbol(tokens[i]);
    }
    return out;
}

GestureCategory categorize_gesture(std::string_view canonical) {
    std::vector<Token> tokens;
    if (!parse_gesture(canonical, tokens) || tokens.empty()) return GestureCategory::directional;

    switch (tokens.front()) {
    case Token::wheel_up:
        return GestureCategory::wheel_up;
    case Token::wheel_down:
        return GestureCategory::wheel_down;
    case Token::rocker_right_left:
    case Token::rocker_left_right:
        return GestureCategory::rocker;
    default:
        return GestureCategory::directional;
    }
}

} // namespace mgh
