#include "gesture_recognizer.h"

namespace mgh {
namespace {

constexpr double kPi = 3.14159265358979323846;

// cos(30°): если новое направление отличается от предыдущего меньше чем
// на 30 градусов, считаем это продолжением того же штриха.
constexpr double kStickyCos = 0.86602540378443864676;

// Ограничение длины цепочки: защита от "бесконечных" жестов.
constexpr std::size_t kMaxTokens = 32;

// Минимальное смещение в пикселях, при котором вообще имеет смысл
// пересчитывать направление.
constexpr double kNoisePx = 0.5;

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
    // Значение 10 по умолчанию даёт ~36 px - комфортный порог,
    // при котором случайные мелкие движения не превращаются в жест.
    return 6 + stroke_length_ * 3;
}

void GestureRecognizer::begin(long x, long y) {
    active_ = true;
    start_x_ = x;
    start_y_ = y;
    last_x_ = x;
    last_y_ = y;
    accum_x_ = 0.0;
    accum_y_ = 0.0;
    tokens_.clear();
}

void GestureRecognizer::cancel() {
    active_ = false;
    accum_x_ = 0.0;
    accum_y_ = 0.0;
    tokens_.clear();
}

bool GestureRecognizer::move(long x, long y) {
    if (!active_) return false;

    const double dx = static_cast<double>(x - last_x_);
    const double dy = static_cast<double>(y - last_y_);
    last_x_ = x;
    last_y_ = y;

    if (std::fabs(dx) < kNoisePx && std::fabs(dy) < kNoisePx) return false;

    accum_x_ += dx;
    accum_y_ += dy;

    const double length = std::sqrt(accum_x_ * accum_x_ + accum_y_ * accum_y_);
    if (length < static_cast<double>(threshold_px())) return false;

    const double up_x = accum_x_;
    const double up_y = -accum_y_; // экранная ось Y смотрит вниз

    Token direction = quantize_direction(up_x, up_y);

    // "Липкое" направление: не дробим один штрих из-за дрожания руки.
    if (!tokens_.empty() && token_is_directional(tokens_.back()) && direction != tokens_.back()) {
        double ux = 0.0, uy = 0.0;
        direction_vector(tokens_.back(), ux, uy);
        const double cosine = (up_x * ux + up_y * uy) / length;
        if (cosine > kStickyCos) direction = tokens_.back();
    }

    accum_x_ = 0.0;
    accum_y_ = 0.0;

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
