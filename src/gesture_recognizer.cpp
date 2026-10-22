#include "gesture_recognizer.h"

namespace mgh {
namespace {

constexpr double kPi = 3.14159265358979323846;

// cos(20°): небольшое дрожание не дробит штрих, но настоящий поворот
// на 45 градусов уже не прячется внутри слишком широкого "липкого" допуска.
constexpr double kStickyCos = 0.93969262078590838405;

// Ограничение длины цепочки: защита от "бесконечных" жестов.
constexpr std::size_t kMaxTokens = 32;

// Минимальное смещение в пикселях, при котором вообще имеет смысл
// пересчитывать направление.
constexpr double kNoisePx = 0.5;

// cos(50°): новое направление явно отклоняется от накопленного штриха.
constexpr double kTurnCos = 0.64278760968653932632;

// Медленный поворот подтверждаем двумя шагами подряд, чтобы случайный выброс
// координат не разрезал жест. Большой шаг (4 px и больше) достаточно явный сам.
constexpr double kMinTurnAccumPx = 2.0;
constexpr double kFastTurnStepPx = 4.0;
constexpr int kTurnVotes = 2;

Token quantize_direction(double dx, double dy_up) {
    const double angle = std::atan2(dy_up, dx) * 180.0 / kPi;
    int sector = static_cast<int>(std::floor((angle + 22.5) / 45.0));
    sector = ((sector % 8) + 8) % 8;

    switch (sector) {
    case 0: return Token::right;
    case 1: return Token::up_right;
    case 2: return Token::up;
    case 3: return Token::up_left;
    case 4: return Token::left;
    case 5: return Token::down_left;
    case 6: return Token::down;
    default: return Token::down_right;
    }
}

void direction_vector(Token token, double& x, double& y_up) {
    constexpr double kDiag = 0.70710678118654752440;
    switch (token) {
    case Token::right:      x = 1.0;  y_up = 0.0;  break;
    case Token::up_right:   x = kDiag; y_up = kDiag; break;
    case Token::up:         x = 0.0;  y_up = 1.0;  break;
    case Token::up_left:    x = -kDiag; y_up = kDiag; break;
    case Token::left:       x = -1.0; y_up = 0.0;  break;
    case Token::down_left:  x = -kDiag; y_up = -kDiag; break;
    case Token::down:       x = 0.0;  y_up = -1.0; break;
    case Token::down_right: x = kDiag; y_up = -kDiag; break;
    default:                x = 0.0;  y_up = 0.0;  break;
    }
}

} // namespace

void GestureRecognizer::set_stroke_length(int value) {
    stroke_length_ = clamp_int(value, 1, 100);
}

int GestureRecognizer::threshold_px() const {
    // Значение 8 по умолчанию даёт 20 px вместо прежних 36 px.
    // У уже существующей настройки 10 порог тоже снижается - до 24 px.
    return 4 + stroke_length_ * 2;
}

int GestureRecognizer::min_segment_px() const {
    // На резком повороте и в конце жеста принимаем штрих чуть короче
    // обычного порога. Это особенно важно для последнего звена цепочки.
    return (threshold_px() * 3) / 5;
}

void GestureRecognizer::begin(long x, long y) {
    active_ = true;
    start_x_ = x;
    start_y_ = y;
    last_x_ = x;
    last_y_ = y;
    accum_x_ = 0.0;
    accum_y_ = 0.0;
    turn_votes_ = 0;
    cardinal_anchor_x_ = x;
    cardinal_anchor_y_ = y;
    cardinal_tokens_.clear();
    tokens_.clear();
}

void GestureRecognizer::cancel() {
    active_ = false;
    accum_x_ = 0.0;
    accum_y_ = 0.0;
    turn_votes_ = 0;
    cardinal_anchor_x_ = 0;
    cardinal_anchor_y_ = 0;
    cardinal_tokens_.clear();
    tokens_.clear();
}

bool GestureRecognizer::move(long x, long y) {
    if (!active_) return false;

    const double dx = static_cast<double>(x - last_x_);
    const double dy = static_cast<double>(y - last_y_);
    last_x_ = x;
    last_y_ = y;

    if (std::fabs(dx) < kNoisePx && std::fabs(dy) < kNoisePx) return false;

    const double step = std::sqrt(dx * dx + dy * dy);
    const double accum_length = std::sqrt(accum_x_ * accum_x_ + accum_y_ * accum_y_);
    bool added = false;

    if (accum_length >= kMinTurnAccumPx && step >= kNoisePx) {
        const double cosine = (accum_x_ * dx + accum_y_ * dy) / (accum_length * step);
        if (cosine < kTurnCos) {
            ++turn_votes_;

            // Быстрый поворот виден сразу. При медленном движении ждём второй
            // шаг в том же направлении: так одиночная дрожь не режет штрих,
            // а поворот по 1 px не склеивает "↓ →" в диагональ "↘".
            if (step >= kFastTurnStepPx || turn_votes_ >= kTurnVotes) {
                // Короткий остаток отбрасываем, но всё равно сбрасываем:
                // при развороте он иначе вычтется из следующего направления.
                if (accum_length >= static_cast<double>(min_segment_px())) {
                    added = emit_direction(accum_x_, accum_y_);
                }
                accum_x_ = 0.0;
                accum_y_ = 0.0;
                turn_votes_ = 0;
            }
        } else {
            turn_votes_ = 0;
        }
    }

    accum_x_ += dx;
    accum_y_ += dy;

    const double length = std::sqrt(accum_x_ * accum_x_ + accum_y_ * accum_y_);
    if (length >= static_cast<double>(threshold_px())) {
        added = emit_direction(accum_x_, accum_y_) || added;
        accum_x_ = 0.0;
        accum_y_ = 0.0;
        turn_votes_ = 0;
    }

    move_cardinal(x, y, std::max(4, threshold_px() * 3 / 4));
    return added;
}

bool GestureRecognizer::finish() {
    if (!active_) return false;
    active_ = false;

    move_cardinal(last_x_, last_y_, std::max(4, min_segment_px() * 3 / 4));

    const double length = std::sqrt(accum_x_ * accum_x_ + accum_y_ * accum_y_);
    if (length < static_cast<double>(min_segment_px())) {
        accum_x_ = 0.0;
        accum_y_ = 0.0;
        turn_votes_ = 0;
        return false;
    }

    const bool added = emit_direction(accum_x_, accum_y_);
    accum_x_ = 0.0;
    accum_y_ = 0.0;
    turn_votes_ = 0;
    return added;
}

void GestureRecognizer::move_cardinal(long x, long y, int distance_px) {
    const double dx = static_cast<double>(x - cardinal_anchor_x_);
    const double dy = static_cast<double>(y - cardinal_anchor_y_);
    if (std::sqrt(dx * dx + dy * dy) < static_cast<double>(distance_px)) return;

    // Проекция на доминирующую ось не создаёт переходных диагоналей и поэтому
    // особенно хорошо подходит для округлых поворотов между U/D/L/R.
    const Token direction = std::fabs(dx) > std::fabs(dy)
        ? (dx > 0.0 ? Token::right : Token::left)
        : (dy > 0.0 ? Token::down : Token::up);

    if (cardinal_tokens_.empty() || cardinal_tokens_.back() != direction) {
        if (cardinal_tokens_.size() < kMaxTokens) cardinal_tokens_.push_back(direction);
    }

    // Даже повтор направления является завершённым отрезком: следующий
    // поворот должен считаться от новой точки, а не от начала всего жеста.
    cardinal_anchor_x_ = x;
    cardinal_anchor_y_ = y;
}

std::vector<std::string> GestureRecognizer::match_candidates() const {
    auto candidates = gesture_match_candidates(tokens_);

    // Если детальный 8-направленный разбор не совпал, пробуем независимую
    // проекцию на U/D/L/R. Для коротких жестов и одиночных диагоналей запасной
    // вариант не нужен и был бы неоднозначным.
    if (cardinal_tokens_.size() < 3) return candidates;

    for (const auto& candidate : gesture_match_candidates(cardinal_tokens_)) {
        bool duplicate = false;
        for (const auto& existing : candidates) {
            if (existing == candidate) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) candidates.push_back(candidate);
    }
    return candidates;
}

bool GestureRecognizer::emit_direction(double accum_x, double accum_y) {
    const double length = std::sqrt(accum_x * accum_x + accum_y * accum_y);
    if (length < kNoisePx) return false;

    const double up_x = accum_x;
    const double up_y = -accum_y; // экранная ось Y смотрит вниз

    Token direction = quantize_direction(up_x, up_y);

    // "Липкое" направление: не дробим один штрих из-за дрожания руки.
    if (!tokens_.empty() && token_is_directional(tokens_.back()) && direction != tokens_.back()) {
        double ux = 0.0, uy = 0.0;
        direction_vector(tokens_.back(), ux, uy);
        const double cosine = (up_x * ux + up_y * uy) / length;
        if (cosine > kStickyCos) direction = tokens_.back();
    }

    // Повтор того же направления не создаёт нового звена - штрих просто длится.
    if (!tokens_.empty() && tokens_.back() == direction) return false;

    return push(direction);
}

bool GestureRecognizer::wheel(int delta) {
    if (delta == 0) return false;
    return push(delta > 0 ? Token::wheel_up : Token::wheel_down);
}

bool GestureRecognizer::rocker(bool right_button_first) {
    return push(right_button_first ? Token::rocker_right_left : Token::rocker_left_right);
}

bool GestureRecognizer::push(Token token) {
    if (tokens_.size() >= kMaxTokens) return false;
    tokens_.push_back(token);
    return true;
}

} // namespace mgh
