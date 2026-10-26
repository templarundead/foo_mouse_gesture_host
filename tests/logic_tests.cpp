#include "gesture_recognizer.h"
#include "gesture_tokens.h"

#include <iostream>
#include <set>

namespace {

int failures = 0;
int trajectory_checks = 0;

#define CHECK_TRUE(condition) check_true((condition), #condition, __FILE__, __LINE__)

void check_true(bool condition, const char* expression, const char* file, int line) {
    if (condition) return;
    ++failures;
    std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
}

struct TestToken {
    mgh::Token token;
    const char* id;
    const char* symbol;
    bool directional;
};

constexpr TestToken kTokens[] = {
    { mgh::Token::up, "U", "\xE2\x86\x91", true },
    { mgh::Token::down, "D", "\xE2\x86\x93", true },
    { mgh::Token::left, "L", "\xE2\x86\x90", true },
    { mgh::Token::right, "R", "\xE2\x86\x92", true },
    { mgh::Token::up_left, "UL", "\xE2\x86\x96", true },
    { mgh::Token::up_right, "UR", "\xE2\x86\x97", true },
    { mgh::Token::down_left, "DL", "\xE2\x86\x99", true },
    { mgh::Token::down_right, "DR", "\xE2\x86\x98", true },
    { mgh::Token::wheel_up, "WU", "W\xE2\x86\x91", false },
    { mgh::Token::wheel_down, "WD", "W\xE2\x86\x93", false },
    { mgh::Token::rocker_right_left, "R+L", "R+L", false },
    { mgh::Token::rocker_left_right, "L+R", "L+R", false },
};

void test_tokens() {
    static_assert(sizeof(kTokens) / sizeof(kTokens[0]) == 12);
    for (const auto& expected : kTokens) {
        CHECK_TRUE(mgh::token_id(expected.token) == expected.id);
        CHECK_TRUE(mgh::token_symbol(expected.token) == expected.symbol);
        CHECK_TRUE(mgh::token_is_directional(expected.token) == expected.directional);

        mgh::Token parsed = mgh::Token::up;
        CHECK_TRUE(mgh::token_from_id(expected.id, parsed));
        CHECK_TRUE(parsed == expected.token);
        CHECK_TRUE(mgh::token_from_id(expected.symbol, parsed));
        CHECK_TRUE(parsed == expected.token);

        const std::string lower = [&] {
            std::string value(expected.id);
            for (char& c : value) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return value;
        }();
        CHECK_TRUE(mgh::token_from_id(lower, parsed));
        CHECK_TRUE(parsed == expected.token);
    }

    std::vector<mgh::Token> tokens;
    CHECK_TRUE(mgh::parse_gesture("", tokens));
    CHECK_TRUE(tokens.empty());
    CHECK_TRUE(mgh::parse_gesture(" \t ", tokens));
    CHECK_TRUE(tokens.empty());
    CHECK_TRUE(mgh::normalize_gesture(" u\tur  R+l ") == "U UR R+L");
    CHECK_TRUE(mgh::normalize_gesture("UR") == "UR");
    CHECK_TRUE(mgh::normalize_gesture("U R") == "U R");

    for (const std::string malformed : {
             "U ???", "U R+", "U  R  +", "U\nR", "\xF0\x28\x8C\x28",
             "WX", "U/R", "R + L", "ULDR", "W U"}) {
        CHECK_TRUE(!mgh::parse_gesture(malformed, tokens));
        CHECK_TRUE(tokens.empty());
        CHECK_TRUE(mgh::normalize_gesture(malformed).empty());
    }

    const std::string arrows = "\xE2\x86\x93 \xE2\x86\x97 \xE2\x86\x96 \xE2\x86\x90";
    CHECK_TRUE(mgh::normalize_gesture(arrows) == "D UR UL L");
    CHECK_TRUE(mgh::format_gesture("D UR UL L", false) == arrows);
    CHECK_TRUE(mgh::format_gesture("D UR UL L", true) == "D UR UL L");
    CHECK_TRUE(mgh::categorize_gesture("WU WU") == mgh::GestureCategory::wheel_up);
    CHECK_TRUE(mgh::categorize_gesture("WD") == mgh::GestureCategory::wheel_down);
    CHECK_TRUE(mgh::categorize_gesture("R+L") == mgh::GestureCategory::rocker);
}

void check_candidates(const std::vector<mgh::Token>& tokens,
                      const std::vector<std::string>& expected) {
    const auto candidates = mgh::gesture_match_candidates(tokens);
    CHECK_TRUE(candidates.size() == expected.size());
    if (candidates.size() != expected.size()) return;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_TRUE(candidates[i] == expected[i]);
    }
    CHECK_TRUE(std::set<std::string>(candidates.begin(), candidates.end()).size() == candidates.size());
}

void test_candidate_order() {
    using mgh::Token;
    check_candidates({}, { "" });
    check_candidates({ Token::down, Token::right, Token::up, Token::left, Token::down },
                     { "D R U L D", "D R U L" });
    check_candidates({ Token::down_right, Token::up_right, Token::up_left,
                       Token::down_left, Token::down },
                     { "DR UR UL DL D", "D R U L D", "D R U L" });
    check_candidates({ Token::up_right }, { "UR" });
    check_candidates({ Token::wheel_up, Token::wheel_up }, { "WU WU" });
}

struct Point {
    double x;
    double y;
};

Point interpolate(Point a, Point b, double t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

void append_line(std::vector<Point>& path, Point target, int steps) {
    if (path.empty()) {
        path.push_back(target);
        return;
    }
    const Point start = path.back();
    for (int i = 1; i <= steps; ++i) {
        path.push_back(interpolate(start, target, static_cast<double>(i) / steps));
    }
}

std::vector<Point> rounded_square(double size, double radius, int corner_steps,
                                  double offset_x, double offset_y) {
    const Point start{ offset_x, offset_y };
    const Point down{ offset_x, offset_y + size };
    const Point right{ offset_x + size, offset_y + size };
    const Point up{ offset_x + size, offset_y };
    const Point left{ offset_x, offset_y };

    std::vector<Point> path{ start };
    const Point down_entry{ down.x, down.y - radius };
    append_line(path, down_entry, 12);
    for (int i = 1; i <= corner_steps; ++i) {
        const double t = static_cast<double>(i) / corner_steps;
        const Point a = interpolate(down_entry, down, t);
        const Point b = interpolate(down, Point{ down.x + radius, down.y }, t);
        path.push_back(interpolate(a, b, t));
    }

    const Point right_entry{ right.x - radius, right.y };
    append_line(path, right_entry, 12);
    for (int i = 1; i <= corner_steps; ++i) {
        const double t = static_cast<double>(i) / corner_steps;
        const Point a = interpolate(right_entry, right, t);
        const Point b = interpolate(right, Point{ right.x, right.y - radius }, t);
        path.push_back(interpolate(a, b, t));
    }

    const Point up_entry{ up.x, up.y + radius };
    append_line(path, up_entry, 12);
    for (int i = 1; i <= corner_steps; ++i) {
        const double t = static_cast<double>(i) / corner_steps;
        const Point a = interpolate(up_entry, up, t);
        const Point b = interpolate(up, Point{ up.x - radius, up.y }, t);
        path.push_back(interpolate(a, b, t));
    }
    append_line(path, left, 12);
    return path;
}

void check_trajectory(const std::vector<Point>& path, std::string_view expected) {
    mgh::GestureRecognizer recognizer;
    recognizer.set_stroke_length(8);
    CHECK_TRUE(recognizer.threshold_px() == 20);
    recognizer.begin(static_cast<long>(std::lround(path.front().x)),
                     static_cast<long>(std::lround(path.front().y)));
    for (std::size_t i = 1; i < path.size(); ++i) {
        recognizer.move(static_cast<long>(std::lround(path[i].x)),
                        static_cast<long>(std::lround(path[i].y)));
    }
    recognizer.finish();
    const auto candidates = recognizer.match_candidates();
    CHECK_TRUE(!candidates.empty());
    CHECK_TRUE(candidates.front() == recognizer.canonical());
    CHECK_TRUE(std::set<std::string>(candidates.begin(), candidates.end()).size() == candidates.size());
    CHECK_TRUE(std::find(candidates.begin(), candidates.end(), expected) != candidates.end());
    ++trajectory_checks;
}

void test_recognizer_trajectories() {
    const std::vector<std::pair<mgh::Token, Point>> directions = {
        { mgh::Token::right, { 70, 0 } }, { mgh::Token::up_right, { 50, -50 } },
        { mgh::Token::up, { 0, -70 } }, { mgh::Token::up_left, { -50, -50 } },
        { mgh::Token::left, { -70, 0 } }, { mgh::Token::down_left, { -50, 50 } },
        { mgh::Token::down, { 0, 70 } }, { mgh::Token::down_right, { 50, 50 } },
    };
    for (const auto& [token, delta] : directions) {
        check_trajectory({ { 200, 200 }, { 200 + delta.x, 200 + delta.y } }, mgh::token_id(token));
    }

    // Детерминированная матрица 4 размеров × 4 радиусов × 4 дискретизаций × 2 сдвигов.
    for (const double size : { 40.0, 55.0, 70.0, 85.0 }) {
        for (const double radius_factor : { 0.0, 3.0, 4.0, 5.0 }) {
            for (const int corner_steps : { 3, 5, 8, 11 }) {
                for (const int offset : { 0, 1 }) {
                    const double radius = radius_factor == 0.0 ? 0.0 : size / radius_factor;
                    check_trajectory(rounded_square(size, radius, corner_steps,
                                                    100.0 + offset * 17.0, 80.0 + offset * 23.0),
                                     "D R U L");
                }
            }
        }
    }
    CHECK_TRUE(trajectory_checks >= 100);
}

} // namespace

int main() {
    test_tokens();
    test_candidate_order();
    test_recognizer_trajectories();

    if (failures != 0) {
        std::cerr << failures << " logic test assertion(s) failed\n";
        return 1;
    }
    std::cout << "Logic tests passed: 12 tokens, candidate order, "
              << trajectory_checks << " deterministic trajectories\n";
    return 0;
}
