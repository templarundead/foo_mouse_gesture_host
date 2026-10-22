#pragma once

#include "gesture_tokens.h"

namespace mgh {

//! Векторный распознаватель направленных жестов.
//!
//! Траектория движения мыши разбивается на отрезки: как только накопленный
//! вектор смещения превышает порог (Stroke Length), его угол квантуется
//! в один из восьми секторов (4 направления + 4 диагонали) и звено
//! добавляется в цепочку. Небольшой "липкий" допуск (20 градусов) не даёт
//! дрожанию руки разрывать одно длинное движение на несколько звеньев, а явный
//! поворот завершает текущий штрих до смешивания его со следующим.
//!
//! Класс ничего не знает про Win32 и про foobar2000 - это чистая логика,
//! что позволяет легко её тестировать и переиспользовать.
class GestureRecognizer {
public:
    //! Длина штриха в "попугаях" (1..100), как в настройках.
    void set_stroke_length(int value);
    int stroke_length() const { return stroke_length_; }

    //! Порог срабатывания в пикселях.
    int threshold_px() const;

    //! Минимальная длина штриха, завершённого поворотом или отпусканием ПКМ.
    int min_segment_px() const;

    //! Начать жест в экранной точке (x, y).
    void begin(long x, long y);

    //! Сбросить состояние без выдачи результата.
    void cancel();

    bool active() const { return active_; }

    //! Накопить движение. Возвращает true, если цепочка пополнилась звеном.
    bool move(long x, long y);

    //! Завершить жест и сохранить последний штрих, даже если он чуть короче
    //! обычного порога. Без этого последнее звено легко потерять на отпускании.
    bool finish();

    //! Добавить звено колеса (delta > 0 - вверх).
    bool wheel(int delta);

    //! Добавить рокерное звено.
    bool rocker(bool right_button_first);

    const std::vector<Token>& tokens() const { return tokens_; }
    //! Независимая проекция траектории на 4 основных направления. Используется
    //! как запасной вариант для округлых составных жестов, после точного разбора.
    const std::vector<Token>& cardinal_tokens() const { return cardinal_tokens_; }
    //! Точный и запасные варианты жеста в порядке приоритета сопоставления.
    std::vector<std::string> match_candidates() const;
    std::string canonical() const { return join_tokens(tokens_); }
    bool empty() const { return tokens_.empty(); }

    long start_x() const { return start_x_; }
    long start_y() const { return start_y_; }

    //! Прямое добавление звена (используется панелью настроек для предпросмотра).
    bool push_token(Token token) { return push(token); }

private:
    bool push(Token token);
    //! Перевести накопленный вектор в звено цепочки.
    bool emit_direction(double accum_x, double accum_y);
    //! Накопить запасной вариант по доминирующей оси (без диагоналей).
    void move_cardinal(long x, long y, int distance_px);

    bool active_ = false;
    int stroke_length_ = 8;

    long start_x_ = 0;
    long start_y_ = 0;
    long last_x_ = 0;
    long last_y_ = 0;

    // Накопленное смещение с момента последнего зафиксированного звена,
    // в экранных координатах (ось Y направлена вниз).
    double accum_x_ = 0.0;
    double accum_y_ = 0.0;

    // Подтверждения медленного поворота (одиночные выбросы игнорируются).
    int turn_votes_ = 0;

    // Состояние запасного 4-направленного разбора.
    long cardinal_anchor_x_ = 0;
    long cardinal_anchor_y_ = 0;
    std::vector<Token> cardinal_tokens_;

    std::vector<Token> tokens_;
};

} // namespace mgh
