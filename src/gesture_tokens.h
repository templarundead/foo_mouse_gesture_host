#pragma once

#include "logic_common.h"

namespace mgh {

//! Одно "звено" жеста.
enum class Token : std::uint8_t {
    up,
    down,
    left,
    right,
    up_left,
    up_right,
    down_left,
    down_right,
    wheel_up,
    wheel_down,
    rocker_right_left, //!< ПКМ зажата, затем клик ЛКМ ("R+L").
    rocker_left_right  //!< ЛКМ зажата, затем клик ПКМ ("L+R").
};

//! Канонический ASCII-идентификатор звена ("U", "UL", "WU", "R+L").
std::string token_id(Token token);

//! Отображение звена стрелкой ("↑", "↖", "W↑", "R+L").
std::string token_symbol(Token token);

//! Разбор канонического идентификатора (без учёта регистра).
bool token_from_id(std::string_view text, Token& out);

//! Является ли звено направлением (а не колесом/рокером).
bool token_is_directional(Token token);

//! Склейка звеньев в каноническую строку через пробел.
std::string join_tokens(const std::vector<Token>& tokens);

//! Варианты для сопоставления с привязками. Первым всегда идёт точный жест.
//! Дополнительные варианты убирают переходные диагонали между основными
//! направлениями и необязательный замыкающий поворот. Это позволяет нарисовать
//! "D R U L" с округлыми углами, не теряя поддержки точных диагональных жестов.
std::vector<std::string> gesture_match_candidates(const std::vector<Token>& tokens);

//! Разбор строки жеста. Пустая строка - корректный жест.
bool parse_gesture(std::string_view gesture, std::vector<Token>& out);

//! Нормализация произвольной записи ("u  d" -> "U D"). При ошибке - пустая строка.
std::string normalize_gesture(std::string_view raw);

//! Отображение жеста: стрелки либо буквы (настройка "Represent direction by the alphabet").
std::string format_gesture(std::string_view canonical, bool alphabet);

//! Категория жеста - используется для группировки строк в списке настроек.
enum class GestureCategory {
    directional,
    wheel_up,
    wheel_down,
    rocker
};

GestureCategory categorize_gesture(std::string_view canonical);

} // namespace mgh
