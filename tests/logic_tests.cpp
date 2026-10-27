#include "binding_index.h"
#include "command_identity.h"
#include "gesture_config.h"
#include "gesture_list_sort.h"
#include "gesture_recognizer.h"
#include "gesture_session.h"
#include "gesture_tokens.h"

#include <chrono>
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

void test_config_roundtrip_and_migration() {
    using mgh::GestureBinding;

    const std::vector<GestureBinding> source = {
        { "U R", "main", "Playback/Next", "main_window",
          "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "", "0.4.2" },
        { "WU WU", "context_ui", "Utilities/\xE5\x88\x86\xE4\xBA\xAB \\ \t\n",
          "playlist", "11111111-2222-3333-4444-555555555555",
          "66666666-7777-8888-9999-aaaaaaaaaaaa", "1.0.3" },
        { "R+L", "context_now_playing", "Now Playing \xE2\x9C\x93", "now_playing", "", "", "2" },
    };
    const std::string serialized = mgh::GestureConfig::serialize(source);
    CHECK_TRUE(serialized.rfind("MGH2\n", 0) == 0);
    const auto parsed = mgh::GestureConfig::deserialize(serialized);
    CHECK_TRUE(parsed.valid);
    CHECK_TRUE(parsed.schema == mgh::BindingSchema::schema_v2);
    CHECK_TRUE(parsed.dropped_rows == 0);
    CHECK_TRUE(parsed.duplicate_rows == 0);
    CHECK_TRUE(parsed.bindings == source);
    CHECK_TRUE(mgh::GestureConfig::serialize(parsed.bindings) == serialized);

    const auto empty_v2 = mgh::GestureConfig::deserialize(mgh::GestureConfig::serialize({}));
    CHECK_TRUE(empty_v2.valid);
    CHECK_TRUE(empty_v2.schema == mgh::BindingSchema::schema_v2);
    CHECK_TRUE(empty_v2.bindings.empty());
    const auto empty_legacy = mgh::GestureConfig::deserialize("");
    CHECK_TRUE(empty_legacy.valid);
    CHECK_TRUE(empty_legacy.schema == mgh::BindingSchema::empty);
    CHECK_TRUE(empty_legacy.bindings.empty());

    const std::string legacy_text =
        "L\tmain\tPlayback/Previous\tmain_window\n"
        "U\tcontext_ui\tUtilities/\xE5\x88\x86\xE4\xBA\xAB\tplaylist\n";
    const auto legacy = mgh::GestureConfig::deserialize(legacy_text);
    CHECK_TRUE(legacy.valid);
    CHECK_TRUE(legacy.schema == mgh::BindingSchema::legacy_v1);
    CHECK_TRUE(legacy.bindings.size() == 2);
    CHECK_TRUE(legacy.bindings[0].command_path == "Playback/Previous");
    CHECK_TRUE(legacy.bindings[1].command_path == "Utilities/\xE5\x88\x86\xE4\xBA\xAB");
    CHECK_TRUE(legacy.bindings[0].command_guid.empty());
    CHECK_TRUE(legacy.bindings[0].node_path.empty());
    const auto migrated = mgh::GestureConfig::deserialize(mgh::GestureConfig::serialize(legacy.bindings));
    CHECK_TRUE(migrated.valid);
    CHECK_TRUE(migrated.schema == mgh::BindingSchema::schema_v2);
    CHECK_TRUE(migrated.bindings == legacy.bindings);

    const auto defaults = mgh::GestureConfig::defaults();
    const std::vector<GestureBinding> expected_defaults = {
        { "L", "main", "Playback/Previous", "main_window" },
        { "R", "main", "Playback/Next", "main_window" },
        { "U", "main", "Playback/Volume up", "main_window" },
        { "D", "main", "Playback/Volume down", "main_window" },
    };
    CHECK_TRUE(defaults.bindings == expected_defaults);
}

void test_config_corruption_and_duplicates() {
    const std::string valid_row = "U\tmain\tPlayback/Play\tmain_window\t\t\t\n";
    const std::string valid_guid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    std::string corrupt = "MGH2\n" + valid_row;
    corrupt += "D\tmain\tOnly three fields\n";
    corrupt += "WHAT\tmain\tPlayback/Next\tmain_window\t\t\t\n";
    corrupt += "D\tunknown\tPlayback/Next\tmain_window\t\t\t\n";
    corrupt += "D\tmain\tPlayback/Next\tunknown\t\t\t\n";
    corrupt += "D\tmain\tBad\\qEscape\tmain_window\t\t\t\n";
    corrupt += "D\tmain\tPlayback/Next\tmain_window\tnot-a-guid\t\t\n";
    corrupt += "D\tmain\tPlayback/Next\tmain_window\t\t" + valid_guid + "\t\n";
    corrupt += "D\tmain\tPlayback/Next\tmain_window\t\t\t0..2\n";
    corrupt += std::string("D\tmain\t") + "\xF0\x28\x8C\x28" + "\tmain_window\t\t\t\n";

    const auto parsed = mgh::GestureConfig::deserialize(corrupt);
    CHECK_TRUE(!parsed.valid);
    CHECK_TRUE(parsed.dropped_rows == 9);
    CHECK_TRUE(parsed.bindings.size() == 1);
    CHECK_TRUE(parsed.bindings.front().command_path == "Playback/Play");
    CHECK_TRUE(parsed.diagnostics.size() == 9);

    const auto unknown = mgh::GestureConfig::deserialize("MGH99\nU\tmain\tX\tmain_window\n");
    CHECK_TRUE(!unknown.valid);
    CHECK_TRUE(unknown.schema == mgh::BindingSchema::unknown);
    CHECK_TRUE(unknown.bindings.empty());

    const std::string duplicates = "MGH2\n" + valid_row + valid_row +
        "U\tmain\tPlayback/Pause\tmain_window\t\t\t\n";
    const auto deduplicated = mgh::GestureConfig::deserialize(duplicates);
    CHECK_TRUE(deduplicated.valid);
    CHECK_TRUE(deduplicated.duplicate_rows == 1);
    CHECK_TRUE(deduplicated.dropped_rows == 0);
    CHECK_TRUE(deduplicated.bindings.size() == 2);
    CHECK_TRUE(deduplicated.bindings[0].command_path == "Playback/Play");
    CHECK_TRUE(deduplicated.bindings[1].command_path == "Playback/Pause");

    CHECK_TRUE(mgh::group_id_from_label("not a group").empty());
    CHECK_TRUE(mgh::place_id_from_label("not a place").empty());
    CHECK_TRUE(std::string(mgh::group_label("not a group")) == "unknown group");
    CHECK_TRUE(std::string(mgh::place_label("not a place")) == "unknown place");
}

void test_command_identity_resolution() {
    const std::string guid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    std::vector<mgh::CatalogEntry> entries(2);
    entries[0].path = "Playback/Renamed";
    entries[0].group = "main";
    entries[0].command_guid = guid;
    entries[0].node_path = "0.1";
    entries[1].path = "Playback/Other";
    entries[1].group = "main";
    entries[1].command_guid = "11111111-2222-3333-4444-555555555555";
    entries[1].node_path = "0.2";

    mgh::GestureBinding binding("R", "main", "Playback/Original", "main_window", guid);
    auto match = mgh::resolve_command(binding, entries);
    CHECK_TRUE(match.status == mgh::CommandResolution::found);
    CHECK_TRUE(match.index == 0);

    entries[1].command_guid = guid;
    match = mgh::resolve_command(binding, entries);
    CHECK_TRUE(match.status == mgh::CommandResolution::ambiguous);
    entries[1].command_guid = "11111111-2222-3333-4444-555555555555";

    binding = mgh::GestureBinding("R", "main", "Playback/Renamed", "main_window", "", "", "0.1");
    match = mgh::resolve_command(binding, entries);
    CHECK_TRUE(match.status == mgh::CommandResolution::found);
    CHECK_TRUE(match.index == 0);
    binding.command_path = "Playback/Wrong label";
    match = mgh::resolve_command(binding, entries);
    CHECK_TRUE(match.status == mgh::CommandResolution::unavailable);

    binding = mgh::GestureBinding("R", "main", "Playback/Renamed", "main_window");
    match = mgh::resolve_command(binding, entries);
    CHECK_TRUE(match.status == mgh::CommandResolution::found);
    CHECK_TRUE(match.index == 0);
    binding.command_path = "Renamed";
    match = mgh::resolve_command(binding, entries);
    CHECK_TRUE(match.status == mgh::CommandResolution::unavailable);

    entries[1].path = entries[0].path;
    entries[1].command_guid.clear();
    entries[0].command_guid.clear();
    binding = mgh::GestureBinding("R", "main", entries[0].path, "main_window");
    match = mgh::resolve_command(binding, entries);
    CHECK_TRUE(match.status == mgh::CommandResolution::ambiguous);

    binding.group = "unknown";
    match = mgh::resolve_command(binding, entries);
    CHECK_TRUE(match.status == mgh::CommandResolution::invalid);
    binding.group = "main";
    binding.place = "unknown";
    match = mgh::resolve_command(binding, entries);
    CHECK_TRUE(match.status == mgh::CommandResolution::invalid);

    mgh::GestureBinding assigned;
    mgh::assign_command_identity(assigned, entries[0]);
    CHECK_TRUE(assigned.command_path == entries[0].path);
    CHECK_TRUE(assigned.command_guid == entries[0].command_guid);
    CHECK_TRUE(assigned.subcommand_guid == entries[0].subcommand_guid);
    CHECK_TRUE(assigned.node_path == entries[0].node_path);
}

std::vector<mgh::GestureBinding> make_benchmark_bindings(std::size_t count) {
    std::vector<mgh::GestureBinding> bindings;
    bindings.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<mgh::Token> tokens;
        std::size_t value = i;
        do {
            tokens.push_back(kTokens[value % 12].token);
            value /= 12;
        } while (value != 0);
        bindings.emplace_back(mgh::join_tokens(tokens), "main", "Command/" + std::to_string(i),
                              "main_window");
    }
    return bindings;
}

bool linear_has_prefix(const std::vector<mgh::GestureBinding>& bindings, std::string_view prefix) {
    for (const auto& binding : bindings) {
        if (std::string_view(binding.gesture).substr(0, prefix.size()) == prefix) return true;
    }
    return false;
}

void test_binding_index_differential() {
    mgh::GestureSettings settings;
    settings.bindings = make_benchmark_bindings(150);
    settings.bindings.emplace_back(settings.bindings[3].gesture, "main", "Duplicate gesture/other row",
                                   "main_window");
    mgh::BindingSnapshot snapshot(settings);

    std::vector<std::string> probes = { "", "missing", "U X", "\xE2\x9C\x93" };
    for (const auto& binding : settings.bindings) {
        probes.push_back(binding.gesture);
        for (std::size_t length = 0; length <= binding.gesture.size(); ++length) {
            probes.push_back(binding.gesture.substr(0, length));
        }
    }

    for (const std::string& probe : probes) {
        std::vector<std::size_t> expected;
        for (std::size_t i = 0; i < settings.bindings.size(); ++i) {
            if (settings.bindings[i].gesture == probe) expected.push_back(i);
        }
        const auto actual = snapshot.exact_matches(probe);
        CHECK_TRUE(actual.size() == expected.size());
        CHECK_TRUE(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
        CHECK_TRUE(snapshot.has_binding(probe) == !expected.empty());
        CHECK_TRUE(snapshot.has_binding_prefix(probe) == linear_has_prefix(settings.bindings, probe));
    }

    mgh::GestureSettings wheel_settings;
    wheel_settings.bindings = {
        { "WU", "main", "WU", "main_window" },
        { "WD", "main", "WD", "main_window" },
        { "WU WU", "main", "WU WU", "main_window" },
        { "WU WD R", "main", "WU WD R", "main_window" },
        { "WD WD WD", "main", "WD WD WD", "main_window" },
        { "R", "main", "R", "main_window" },
    };
    const mgh::BindingSnapshot wheel_snapshot(wheel_settings);
    std::vector<mgh::WheelSequenceKey> level{ 0 };
    for (std::size_t length = 1; length <= 6; ++length) {
        std::vector<mgh::WheelSequenceKey> next;
        for (const auto base : level) {
            for (const bool up : { false, true }) {
                const auto key = mgh::append_wheel_sequence(base, up);
                next.push_back(key);
                std::vector<mgh::Token> tokens;
                for (std::size_t i = 0; i < length; ++i) {
                    const bool down = (static_cast<std::uint32_t>(key) & (std::uint32_t{1} << i)) != 0;
                    tokens.push_back(down ? mgh::Token::wheel_down : mgh::Token::wheel_up);
                }
                const std::string gesture = mgh::join_tokens(tokens);
                const auto match = wheel_snapshot.wheel_match(key);
                bool exact = false;
                for (const auto& binding : wheel_settings.bindings) {
                    if (binding.gesture == gesture) exact = true;
                }
                CHECK_TRUE(match.exact == exact);
                CHECK_TRUE(match.prefix == linear_has_prefix(wheel_settings.bindings, gesture));
            }
        }
        level = std::move(next);
    }

    mgh::WheelSequenceKey long_sequence = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        long_sequence = mgh::append_wheel_sequence(long_sequence, true);
    }
    CHECK_TRUE(mgh::wheel_sequence_length(long_sequence) == 32);
    CHECK_TRUE(mgh::append_wheel_sequence(long_sequence, false) == mgh::kInvalidWheelSequence);
    CHECK_TRUE(!wheel_snapshot.wheel_match(mgh::kInvalidWheelSequence).prefix);
}

std::wstring ascii_wide_for_sort(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

mgh::GestureListRow make_sort_row(const mgh::GestureBinding& binding, int binding_index) {
    mgh::GestureListRow row;
    row.binding_index = binding_index;
    row.gesture = ascii_wide_for_sort(binding.gesture);
    row.command = ascii_wide_for_sort(binding.group + ": " + binding.command_path);
    row.place = ascii_wide_for_sort(binding.place);
    mgh::prepare_gesture_list_row(row, binding);
    return row;
}

std::vector<mgh::GestureListRow> make_sort_rows(const std::vector<mgh::GestureBinding>& bindings) {
    std::vector<mgh::GestureListRow> rows;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        rows.push_back(make_sort_row(bindings[i], static_cast<int>(i)));
    }
    return rows;
}

std::vector<int> sort_binding_order(const std::vector<mgh::GestureListRow>& rows) {
    std::vector<int> order;
    for (const auto& row : rows) order.push_back(row.binding_index);
    return order;
}

std::vector<int> data_binding_order(const std::vector<mgh::GestureListRow>& rows) {
    std::vector<int> order;
    for (const auto& row : rows) {
        if (!row.is_header) order.push_back(row.binding_index);
    }
    return order;
}

std::vector<mgh::GestureCategory> group_header_categories(const std::vector<mgh::GestureListRow>& rows) {
    std::vector<mgh::GestureCategory> categories;
    for (const auto& row : rows) {
        if (row.is_header) categories.push_back(row.gesture_category);
    }
    return categories;
}

void test_gesture_list_sort_state_and_gesture() {
    CHECK_TRUE(mgh::gesture_sort_column_from_index(0) == mgh::GestureSortColumn::gesture);
    CHECK_TRUE(mgh::gesture_sort_column_from_index(1) == mgh::GestureSortColumn::command);
    CHECK_TRUE(mgh::gesture_sort_column_from_index(2) == mgh::GestureSortColumn::place);
    CHECK_TRUE(mgh::gesture_sort_column_from_index(-1) == mgh::GestureSortColumn::none);
    CHECK_TRUE(mgh::gesture_sort_column_from_index(3) == mgh::GestureSortColumn::none);

    mgh::GestureSortState state;
    CHECK_TRUE(state.column == mgh::GestureSortColumn::none);
    state = mgh::next_gesture_sort_state(state, mgh::GestureSortColumn::gesture);
    CHECK_TRUE((state == mgh::GestureSortState{ mgh::GestureSortColumn::gesture,
                                                mgh::GestureSortDirection::ascending }));
    state = mgh::next_gesture_sort_state(state, mgh::GestureSortColumn::gesture);
    CHECK_TRUE((state == mgh::GestureSortState{ mgh::GestureSortColumn::gesture,
                                                mgh::GestureSortDirection::descending }));
    state = mgh::next_gesture_sort_state(state, mgh::GestureSortColumn::gesture);
    CHECK_TRUE(state == mgh::GestureSortState{});
    state = mgh::next_gesture_sort_state(state, mgh::GestureSortColumn::place);
    CHECK_TRUE(state.column == mgh::GestureSortColumn::place);
    state = mgh::next_gesture_sort_state(state, mgh::GestureSortColumn::command);
    CHECK_TRUE((state == mgh::GestureSortState{ mgh::GestureSortColumn::command,
                                                mgh::GestureSortDirection::ascending }));

    const std::vector<mgh::GestureBinding> bindings = {
        { "R+L", "main", "Command/Rocker", "now_playing" },
        { "D", "main", "Command/Delta", "main_window" },
        { "WU", "main", "Command/Wheel up", "playlist" },
        { "U R", "main", "Command/Up right", "main_window" },
        { "U", "main", "Command/Up", "playlist" },
        { "WD", "main", "Command/Wheel down", "now_playing" },
        { "UL", "main", "Command/Up left", "main_window" },
    };
    const auto manual = make_sort_rows(bindings);
    const auto original_order = sort_binding_order(manual);
    const auto ascending = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::gesture, mgh::GestureSortDirection::ascending });
    const auto descending = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::gesture, mgh::GestureSortDirection::descending });
    CHECK_TRUE(sort_binding_order(ascending) == std::vector<int>({ 4, 3, 1, 6, 2, 5, 0 }));
    CHECK_TRUE(sort_binding_order(descending) == std::vector<int>({ 0, 5, 2, 6, 1, 3, 4 }));
    CHECK_TRUE(sort_binding_order(manual) == original_order);
    CHECK_TRUE(sort_binding_order(mgh::make_gesture_list_view(manual, {})) == original_order);

    state = {};
    for (int cycle = 0; cycle < 3; ++cycle) {
        state = mgh::next_gesture_sort_state(state, mgh::GestureSortColumn::gesture);
        state = mgh::next_gesture_sort_state(state, mgh::GestureSortColumn::gesture);
        state = mgh::next_gesture_sort_state(state, mgh::GestureSortColumn::gesture);
        CHECK_TRUE(state.column == mgh::GestureSortColumn::none);
        CHECK_TRUE(sort_binding_order(mgh::make_gesture_list_view(manual, state)) == original_order);
    }
}

void test_gesture_list_sort_command_place_and_ties() {
    std::vector<mgh::GestureBinding> bindings = {
        { "U", "main", "Command/Beta", "playlist" },
        { "D", "main", "Command/Alpha", "main_window" },
        { "R", "main", "Command/Gamma", "now_playing" },
    };
    auto manual = make_sort_rows(bindings);
    manual[0].command = L"main menu: beta";
    manual[1].command = L"Main menu: Alpha";
    manual[2].command = L"main menu: Gamma";
    manual[0].place = L"Playlist";
    manual[1].place = L"Main Window";
    manual[2].place = L"Now Playing";

    const auto command_asc = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::command, mgh::GestureSortDirection::ascending });
    const auto command_desc = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::command, mgh::GestureSortDirection::descending });
    CHECK_TRUE(sort_binding_order(command_asc) == std::vector<int>({ 1, 0, 2 }));
    CHECK_TRUE(sort_binding_order(command_desc) == std::vector<int>({ 2, 0, 1 }));

    const auto place_asc = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::place, mgh::GestureSortDirection::ascending });
    const auto place_desc = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::place, mgh::GestureSortDirection::descending });
    CHECK_TRUE(sort_binding_order(place_asc) == std::vector<int>({ 1, 2, 0 }));
    CHECK_TRUE(sort_binding_order(place_desc) == std::vector<int>({ 0, 2, 1 }));

    bindings = {
        { "U", "main", "Same", "main_window" },
        { "D", "main", "Same", "main_window" },
        { "U", "main", "Same", "main_window" },
    };
    manual = make_sort_rows(bindings);
    const auto gesture_asc = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::gesture, mgh::GestureSortDirection::ascending });
    const auto gesture_desc = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::gesture, mgh::GestureSortDirection::descending });
    CHECK_TRUE(sort_binding_order(gesture_asc) == std::vector<int>({ 0, 2, 1 }));
    CHECK_TRUE(sort_binding_order(gesture_desc) == std::vector<int>({ 1, 0, 2 }));

    const auto command_ties = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::command, mgh::GestureSortDirection::descending });
    const auto place_ties = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::place, mgh::GestureSortDirection::descending });
    CHECK_TRUE(sort_binding_order(command_ties) == std::vector<int>({ 0, 1, 2 }));
    CHECK_TRUE(sort_binding_order(place_ties) == std::vector<int>({ 0, 1, 2 }));

    auto left = make_sort_row({ "U", "main", "A", "main_window" }, 0);
    auto right = make_sort_row({ "U", "main", "B", "main_window" }, 1);
    left.command = right.command = L"Same label";
    CHECK_TRUE(mgh::compare_gesture_list_rows(left, right, mgh::GestureSortColumn::command) < 0);
    left.command_path = right.command_path = "Same path";
    left.group_id = "a";
    right.group_id = "b";
    CHECK_TRUE(mgh::compare_gesture_list_rows(left, right, mgh::GestureSortColumn::command) < 0);
    left.place = right.place = L"Same place";
    left.place_id = "a";
    right.place_id = "b";
    CHECK_TRUE(mgh::compare_gesture_list_rows(left, right, mgh::GestureSortColumn::place) < 0);
}

void test_gesture_list_sort_view_and_selection() {
    const std::vector<mgh::GestureBinding> bindings = {
        { "WU", "main", "Command/Wheel", "playlist" },
        { "U", "main", "Command/Up", "main_window" },
        { "R+L", "main", "Command/Rocker", "now_playing" },
        { "D", "main", "Command/Down", "main_window" },
    };
    const auto original_bindings = bindings;
    const auto manual = make_sort_rows(bindings);
    const auto original_order = sort_binding_order(manual);
    const std::vector<mgh::GestureSortState> states = {
        {},
        { mgh::GestureSortColumn::gesture, mgh::GestureSortDirection::ascending },
        { mgh::GestureSortColumn::gesture, mgh::GestureSortDirection::descending },
        { mgh::GestureSortColumn::command, mgh::GestureSortDirection::ascending },
        { mgh::GestureSortColumn::command, mgh::GestureSortDirection::descending },
        { mgh::GestureSortColumn::place, mgh::GestureSortDirection::ascending },
        { mgh::GestureSortColumn::place, mgh::GestureSortDirection::descending },
    };

    for (const auto state : states) {
        const auto view = mgh::make_gesture_list_view(manual, state);
        auto actual_indices = sort_binding_order(view);
        auto expected_indices = original_order;
        std::sort(actual_indices.begin(), actual_indices.end());
        std::sort(expected_indices.begin(), expected_indices.end());
        CHECK_TRUE(actual_indices == expected_indices);
        CHECK_TRUE(sort_binding_order(manual) == original_order);
        CHECK_TRUE(bindings == original_bindings);

        for (std::size_t binding = 0; binding < bindings.size(); ++binding) {
            const int binding_index = static_cast<int>(binding);
            const int display = mgh::gesture_row_for_binding(view, binding_index);
            CHECK_TRUE(display >= 0);
            if (display >= 0) {
                CHECK_TRUE(view[static_cast<std::size_t>(display)].binding_index == binding_index);
                CHECK_TRUE(bindings[static_cast<std::size_t>(binding_index)] == original_bindings[binding]);
            }
        }
    }

    const std::vector<mgh::GestureListRow> empty;
    for (const auto state : states) {
        CHECK_TRUE(mgh::make_gesture_list_view(empty, state).empty());
        CHECK_TRUE(mgh::gesture_row_for_binding(empty, 0) == -1);
    }
    const auto single = make_sort_rows({ { "U", "main", "Only", "main_window" } });
    for (const auto state : states) {
        const auto view = mgh::make_gesture_list_view(single, state);
        CHECK_TRUE(view.size() == 1);
        CHECK_TRUE(mgh::gesture_row_for_binding(view, 0) == 0);
    }
}

void test_gesture_list_grouping() {
    const std::vector<mgh::GestureBinding> bindings = {
        { "U", "main", "Command/Zulu", "main_window" },
        { "WU", "main", "Command/Wheel up", "playlist" },
        { "D", "main", "Command/Alpha", "main_window" },
        { "R+L", "main", "Command/Rocker", "now_playing" },
        { "WD", "main", "Command/Wheel down", "playlist" },
    };
    const auto original_bindings = bindings;
    const auto manual = make_sort_rows(bindings);
    const auto original_order = sort_binding_order(manual);
    const std::vector<mgh::GestureCategory> expected_headers = {
        mgh::GestureCategory::directional,
        mgh::GestureCategory::wheel_up,
        mgh::GestureCategory::wheel_down,
        mgh::GestureCategory::rocker,
    };

    const auto grouped_manual = mgh::make_gesture_list_view(manual, {}, true);
    CHECK_TRUE(grouped_manual.size() == bindings.size() + expected_headers.size());
    CHECK_TRUE(group_header_categories(grouped_manual) == expected_headers);
    CHECK_TRUE(data_binding_order(grouped_manual) == std::vector<int>({ 0, 2, 1, 4, 3 }));

    const auto command_ascending = mgh::make_gesture_list_view(
        manual, { mgh::GestureSortColumn::command, mgh::GestureSortDirection::ascending }, true);
    CHECK_TRUE(group_header_categories(command_ascending) == expected_headers);
    CHECK_TRUE(data_binding_order(command_ascending) == std::vector<int>({ 2, 0, 1, 4, 3 }));

    const std::vector<mgh::GestureSortState> states = {
        {},
        { mgh::GestureSortColumn::gesture, mgh::GestureSortDirection::ascending },
        { mgh::GestureSortColumn::gesture, mgh::GestureSortDirection::descending },
        { mgh::GestureSortColumn::command, mgh::GestureSortDirection::ascending },
        { mgh::GestureSortColumn::command, mgh::GestureSortDirection::descending },
        { mgh::GestureSortColumn::place, mgh::GestureSortDirection::ascending },
        { mgh::GestureSortColumn::place, mgh::GestureSortDirection::descending },
    };

    for (const auto state : states) {
        const auto grouped = mgh::make_gesture_list_view(manual, state, true);
        CHECK_TRUE(group_header_categories(grouped) == expected_headers);
        const auto data_order = data_binding_order(grouped);
        CHECK_TRUE(data_order.size() == bindings.size());
        CHECK_TRUE(std::set<int>(data_order.begin(), data_order.end()).size() == bindings.size());
        for (const auto& row : grouped) {
            if (row.is_header) {
                CHECK_TRUE(row.binding_index == -1);
            } else {
                CHECK_TRUE(row.binding_index >= 0);
            }
        }
        for (std::size_t binding = 0; binding < bindings.size(); ++binding) {
            const int display = mgh::gesture_row_for_binding(grouped, static_cast<int>(binding));
            CHECK_TRUE(display >= 0);
            if (display >= 0) {
                CHECK_TRUE(!grouped[static_cast<std::size_t>(display)].is_header);
                CHECK_TRUE(grouped[static_cast<std::size_t>(display)].binding_index ==
                           static_cast<int>(binding));
            }
        }

        const auto flat = mgh::make_gesture_list_view(manual, state, false);
        CHECK_TRUE(flat.size() == manual.size());
        CHECK_TRUE(group_header_categories(flat).empty());
        for (const auto& row : flat) {
            CHECK_TRUE(!row.is_header);
            CHECK_TRUE(row.binding_index >= 0);
            if (row.binding_index >= 0) {
                CHECK_TRUE(row.gesture == manual[static_cast<std::size_t>(row.binding_index)].gesture);
            }
        }
        for (const auto category : expected_headers) {
            std::vector<int> expected_group_order;
            std::vector<int> actual_group_order;
            for (const auto& row : flat) {
                if (row.gesture_category == category) expected_group_order.push_back(row.binding_index);
            }
            for (const auto& row : grouped) {
                if (!row.is_header && row.gesture_category == category) {
                    actual_group_order.push_back(row.binding_index);
                }
            }
            CHECK_TRUE(actual_group_order == expected_group_order);
        }
        CHECK_TRUE(sort_binding_order(manual) == original_order);
        CHECK_TRUE(bindings == original_bindings);
    }

    const std::vector<mgh::GestureListRow> directional_only = { manual[0], manual[2] };
    const auto directional_grouped = mgh::make_gesture_list_view(directional_only, {}, true);
    CHECK_TRUE(directional_grouped.size() == 3);
    CHECK_TRUE(group_header_categories(directional_grouped) ==
               std::vector<mgh::GestureCategory>({ mgh::GestureCategory::directional }));
    CHECK_TRUE(data_binding_order(directional_grouped) == std::vector<int>({ 0, 2 }));

    const std::vector<mgh::GestureListRow> empty;
    CHECK_TRUE(mgh::make_gesture_list_view(empty, {}, true).empty());
    CHECK_TRUE(mgh::make_gesture_list_view(empty, {}, false).empty());
}

mgh::CatalogEntry make_editor_catalog_entry(const char* path, const char* group, const char* guid,
                                             bool selectable = true) {
    mgh::CatalogEntry entry;
    entry.path = path;
    entry.group = group;
    entry.command_guid = guid;
    entry.selectable = selectable;
    return entry;
}

void test_editor_command_state() {
    const auto play = make_editor_catalog_entry("Playback/Play", "main", "guid-play");
    const auto next = make_editor_catalog_entry("Playback/Next", "main", "guid-next");
    const std::vector<mgh::CatalogEntry> main_catalog = { play, next };

    mgh::GestureBinding draft{ "U", "main", "", "main_window" };
    auto status = mgh::validate_editor_command(draft, main_catalog, -1);
    CHECK_TRUE(status.state == mgh::GestureEditorCommandState::none);
    CHECK_TRUE(!status.is_selectable());

    mgh::select_editor_command(draft, main_catalog, 0);
    status = mgh::validate_editor_command(draft, main_catalog, 0);
    CHECK_TRUE(status.state == mgh::GestureEditorCommandState::valid);
    CHECK_TRUE(status.is_selectable());
    CHECK_TRUE(draft.command_path == "Playback/Play");

    const std::vector<mgh::CatalogEntry> conflict_catalog = { play, play };
    mgh::select_editor_command(draft, conflict_catalog, 0);
    status = mgh::validate_editor_command(draft, conflict_catalog, 0);
    CHECK_TRUE(status.state == mgh::GestureEditorCommandState::conflict);
    CHECK_TRUE(!status.is_selectable());
    CHECK_TRUE(!status.message.empty());

    mgh::GestureBinding missing{ "D", "main", "Missing", "main_window", "guid-missing" };
    auto synthetic = make_editor_catalog_entry("Missing", "main", "guid-missing", false);
    const std::vector<mgh::CatalogEntry> unavailable_catalog = { synthetic };
    status = mgh::validate_editor_command(missing, unavailable_catalog, 0);
    CHECK_TRUE(status.state == mgh::GestureEditorCommandState::unavailable);
    CHECK_TRUE(!status.is_selectable());

    std::vector<mgh::CatalogEntry> synthetic_conflict_catalog = { play, play, play };
    auto conflict_synthetic = synthetic_conflict_catalog.back();
    conflict_synthetic.selectable = false;
    synthetic_conflict_catalog.back() = conflict_synthetic;
    mgh::GestureBinding conflicting{ "R", "main", "Playback/Play", "main_window", "guid-play" };
    status = mgh::validate_editor_command(conflicting, synthetic_conflict_catalog, 2);
    CHECK_TRUE(status.state == mgh::GestureEditorCommandState::conflict);
    CHECK_TRUE(!status.is_selectable());

    auto context_play = make_editor_catalog_entry("Playback/Play (context)", "context_ui", "guid-play");
    const std::vector<mgh::CatalogEntry> context_catalog = { context_play };
    draft = conflicting;
    const int preserved = mgh::change_editor_group(draft, "context_ui", context_catalog);
    CHECK_TRUE(preserved == 0);
    CHECK_TRUE(draft.group == "context_ui");
    CHECK_TRUE(draft.command_path == "Playback/Play (context)");
    status = mgh::validate_editor_command(draft, context_catalog, preserved);
    CHECK_TRUE(status.is_selectable());

    const int cleared = mgh::change_editor_group(draft, "context_ui", { next });
    CHECK_TRUE(cleared == -1);
    CHECK_TRUE(draft.command_path.empty());
    CHECK_TRUE(draft.command_guid.empty());
    status = mgh::validate_editor_command(draft, { next }, cleared);
    CHECK_TRUE(status.state == mgh::GestureEditorCommandState::none);
    CHECK_TRUE(!status.is_selectable());

    draft.group = "main";
    CHECK_TRUE(mgh::change_editor_group(draft, "main", main_catalog) == -1);
    CHECK_TRUE(draft.command_path.empty());
}

void test_editor_state_and_duplicates() {
    const mgh::GestureEditorState idle;
    CHECK_TRUE(idle.mode == mgh::GestureEditorMode::idle);
    CHECK_TRUE(!idle.dirty);

    auto editor = mgh::make_new_editor_state();
    CHECK_TRUE(editor.mode == mgh::GestureEditorMode::new_binding);
    CHECK_TRUE(editor.binding_index == -1);
    CHECK_TRUE(!editor.dirty);
    CHECK_TRUE(editor.draft.group == "main");
    CHECK_TRUE(editor.draft.place == "main_window");

    const auto play = make_editor_catalog_entry("Playback/Play", "main", "guid-play");
    editor.draft.gesture = "U";
    mgh::select_editor_command(editor.draft, { play }, 0);
    mgh::refresh_editor_dirty(editor);
    CHECK_TRUE(editor.dirty);

    std::vector<mgh::GestureBinding> bindings;
    CHECK_TRUE(!mgh::has_exact_binding_duplicate(bindings, editor.draft));
    bindings.push_back(editor.draft);
    editor = mgh::make_edit_editor_state(bindings.back(), 0);
    CHECK_TRUE(editor.mode == mgh::GestureEditorMode::edit);
    CHECK_TRUE(editor.binding_index == 0);
    CHECK_TRUE(!editor.dirty);

    // Повторный Add видит дубликат; без явного подтверждения размер не меняется.
    bool confirm_duplicate = false;
    if (mgh::has_exact_binding_duplicate(bindings, editor.draft) && confirm_duplicate) {
        bindings.push_back(editor.draft);
    }
    CHECK_TRUE(mgh::has_exact_binding_duplicate(bindings, editor.draft));
    CHECK_TRUE(bindings.size() == 1);

    auto different_command = editor.draft;
    different_command.command_path = "Playback/Next";
    CHECK_TRUE(!mgh::has_exact_binding_duplicate(bindings, different_command));

    auto same_user_fields = editor.draft;
    same_user_fields.command_guid = "other-guid";
    same_user_fields.node_path = "9.8.7";
    CHECK_TRUE(mgh::has_exact_binding_duplicate(bindings, same_user_fields));

    editor.draft.gesture = "D";
    mgh::refresh_editor_dirty(editor);
    CHECK_TRUE(editor.dirty);
    mgh::discard_editor_changes(editor);
    CHECK_TRUE(!editor.dirty);
    CHECK_TRUE(editor.draft == editor.baseline);
    CHECK_TRUE(editor.draft.gesture == "U");

    // Явное подтверждение разрешает полный дубликат и переводит редактор в новый Edit.
    confirm_duplicate = true;
    if (mgh::has_exact_binding_duplicate(bindings, editor.draft) && confirm_duplicate) {
        bindings.push_back(editor.draft);
        editor = mgh::make_edit_editor_state(bindings.back(), 1);
    }
    CHECK_TRUE(bindings.size() == 2);
    CHECK_TRUE(editor.mode == mgh::GestureEditorMode::edit);
    CHECK_TRUE(editor.binding_index == 1);
}

void test_editor_guard_decisions() {
    std::vector<mgh::GestureBinding> bindings = {
        { "U", "main", "Command/Up", "main_window" },
        { "D", "main", "Command/Down", "main_window" },
    };

    auto editor = mgh::make_edit_editor_state(bindings[0], 0);
    int selected_binding = 0;
    const auto request_selection = [&](int target) {
        if (editor.dirty || target < 0 || target >= static_cast<int>(bindings.size())) return false;
        selected_binding = target;
        editor = mgh::make_edit_editor_state(bindings[static_cast<std::size_t>(target)], target);
        return true;
    };

    editor.draft.gesture = "R";
    mgh::refresh_editor_dirty(editor);
    CHECK_TRUE(editor.dirty);

    // Cancel: guard отклоняет переход до изменения selection или редактора.
    const auto cancelled_editor = editor;
    CHECK_TRUE(!request_selection(1));
    CHECK_TRUE(selected_binding == 0);
    CHECK_TRUE(editor.draft == cancelled_editor.draft);
    CHECK_TRUE(editor.baseline == cancelled_editor.baseline);
    CHECK_TRUE(bindings[0].gesture == "U");

    // Save: коммитим именно edit(binding_index), затем тот же переход разрешён.
    bindings[static_cast<std::size_t>(editor.binding_index)] = editor.draft;
    editor = mgh::make_edit_editor_state(bindings[0], 0);
    CHECK_TRUE(request_selection(1));
    CHECK_TRUE(bindings[0].gesture == "R");
    CHECK_TRUE(selected_binding == 1);
    CHECK_TRUE(editor.binding_index == 1);
    CHECK_TRUE(!editor.dirty);

    // Discard: baseline восстанавливается без записи draft в settings.
    editor = mgh::make_edit_editor_state(bindings[0], 0);
    selected_binding = 0;
    editor.draft.gesture = "L";
    mgh::refresh_editor_dirty(editor);
    CHECK_TRUE(!request_selection(1));
    mgh::discard_editor_changes(editor);
    CHECK_TRUE(!editor.dirty);
    CHECK_TRUE(editor.draft.gesture == "R");
    CHECK_TRUE(bindings[0].gesture == "R");
    CHECK_TRUE(request_selection(1));

    // Возврат к baseline сам снимает dirty и не требует guard для навигации.
    editor.draft.gesture = "L";
    mgh::refresh_editor_dirty(editor);
    CHECK_TRUE(editor.dirty);
    editor.draft = editor.baseline;
    mgh::refresh_editor_dirty(editor);
    CHECK_TRUE(!editor.dirty);
}

mgh::BindingSnapshot make_session_snapshot(const std::vector<std::string>& gestures) {
    mgh::GestureSettings settings;
    for (const auto& gesture : gestures) {
        settings.bindings.emplace_back(gesture, "main", "Command/" + gesture, "main_window");
    }
    return mgh::BindingSnapshot(settings);
}

mgh::SessionEvent session_event(mgh::SessionEventKind kind, long x = 100, long y = 100,
                                bool over_main = true, int wheel_delta = 0) {
    mgh::SessionEvent event;
    event.kind = kind;
    event.point = { x, y };
    event.over_main_window = over_main;
    event.wheel_delta = wheel_delta;
    return event;
}

void test_session_idle_drag_and_reconcile() {
    const auto bindings = make_session_snapshot({});
    mgh::GestureSession session;
    CHECK_TRUE(session.state() == mgh::GestureSessionState::idle);

    auto result = session.handle_event(session_event(mgh::SessionEventKind::right_down, 100, 100, false), bindings);
    CHECK_TRUE(!result.swallow);
    CHECK_TRUE(result.action_count == 0);
    CHECK_TRUE(session.state() == mgh::GestureSessionState::idle);

    result = session.handle_event(session_event(mgh::SessionEventKind::right_down), bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(result.action_count == 1);
    CHECK_TRUE(result.actions[0].kind == mgh::SessionActionKind::begin);
    CHECK_TRUE(session.state() == mgh::GestureSessionState::right_down);

    result = session.handle_event(session_event(mgh::SessionEventKind::mouse_move, 102, 100), bindings);
    CHECK_TRUE(!result.swallow);
    CHECK_TRUE(result.action_count == 1);
    CHECK_TRUE(result.actions[0].kind == mgh::SessionActionKind::move);
    CHECK_TRUE(!session.right_moved());

    result = session.handle_event(session_event(mgh::SessionEventKind::right_up, 102, 100), bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(result.action_count == 1);
    CHECK_TRUE(result.actions[0].kind == mgh::SessionActionKind::finish);
    CHECK_TRUE(result.actions[0].was_tap);
    CHECK_TRUE(result.actions[0].was_suppressed);
    CHECK_TRUE(result.actions[0].right_down_point.x == 100);
    CHECK_TRUE(session.state() == mgh::GestureSessionState::idle);

    session.handle_event(session_event(mgh::SessionEventKind::right_down), bindings);
    session.handle_event(session_event(mgh::SessionEventKind::mouse_move, 140, 100), bindings);
    CHECK_TRUE(session.right_moved());
    result = session.handle_event(session_event(mgh::SessionEventKind::right_up, 140, 100), bindings);
    CHECK_TRUE(!result.actions[0].was_tap);

    session.handle_event(session_event(mgh::SessionEventKind::right_down), bindings);
    result = session.reconcile_buttons(false, false, bindings);
    CHECK_TRUE(result.action_count == 1);
    CHECK_TRUE(result.actions[0].kind == mgh::SessionActionKind::finish);
    CHECK_TRUE(session.state() == mgh::GestureSessionState::idle);
}

void test_session_rocker() {
    const auto bindings = make_session_snapshot({ "R+L", "L+R" });
    mgh::GestureSession session;
    session.handle_event(session_event(mgh::SessionEventKind::right_down), bindings);
    auto result = session.handle_event(session_event(mgh::SessionEventKind::left_down, 110, 100), bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(session.state() == mgh::GestureSessionState::rocker_right_left);
    result = session.handle_event(session_event(mgh::SessionEventKind::left_up, 110, 100), bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(result.action_count == 1);
    CHECK_TRUE(result.actions[0].kind == mgh::SessionActionKind::emit_rocker);
    CHECK_TRUE(result.actions[0].rocker_token == mgh::Token::rocker_right_left);
    result = session.handle_event(session_event(mgh::SessionEventKind::right_up), bindings);
    CHECK_TRUE(result.actions[0].kind == mgh::SessionActionKind::finish);

    session.reset();
    result = session.handle_event(session_event(mgh::SessionEventKind::left_down), bindings);
    CHECK_TRUE(!result.swallow);
    result = session.handle_event(session_event(mgh::SessionEventKind::right_down), bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(result.action_count == 0);
    CHECK_TRUE(session.state() == mgh::GestureSessionState::rocker_left_right);
    result = session.handle_event(session_event(mgh::SessionEventKind::right_up), bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(result.action_count == 1);
    CHECK_TRUE(result.actions[0].kind == mgh::SessionActionKind::emit_rocker);
    CHECK_TRUE(result.actions[0].rocker_token == mgh::Token::rocker_left_right);
    result = session.handle_event(session_event(mgh::SessionEventKind::left_up), bindings);
    CHECK_TRUE(!result.swallow);
}

void test_session_wheel_and_fractional_delta() {
    const auto bindings = make_session_snapshot({ "WU", "WD" });
    mgh::GestureSession session;
    session.handle_event(session_event(mgh::SessionEventKind::right_down), bindings);
    CHECK_TRUE(session.handle_event(session_event(mgh::SessionEventKind::wheel, 100, 100, true, 30), bindings).action_count == 0);
    CHECK_TRUE(session.wheel_accumulator() == 30);
    CHECK_TRUE(session.handle_event(session_event(mgh::SessionEventKind::wheel, 100, 100, true, 30), bindings).action_count == 0);
    CHECK_TRUE(session.wheel_accumulator() == 60);
    auto result = session.handle_event(session_event(mgh::SessionEventKind::wheel, 100, 100, true, 60), bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(result.action_count == 1);
    CHECK_TRUE(result.actions[0].kind == mgh::SessionActionKind::emit_wheel);
    CHECK_TRUE(mgh::wheel_sequence_length(result.actions[0].wheel_sequence) == 1);
    CHECK_TRUE(session.wheel_accumulator() == 0);
    CHECK_TRUE(session.state() == mgh::GestureSessionState::wheel);

    result = session.handle_event(session_event(mgh::SessionEventKind::wheel, 100, 100, true, 1200), bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(result.action_count == 4);
    CHECK_TRUE(session.wheel_accumulator() == 0);
    result = session.handle_event(session_event(mgh::SessionEventKind::right_up), bindings);
    CHECK_TRUE(result.actions[0].wheel_used);

    const auto sequence_bindings = make_session_snapshot({ "WU WU" });
    session.reset();
    session.handle_event(session_event(mgh::SessionEventKind::right_down), sequence_bindings);
    result = session.handle_event(session_event(mgh::SessionEventKind::wheel, 100, 100, true, 120), sequence_bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(result.action_count == 0);
    result = session.handle_event(session_event(mgh::SessionEventKind::wheel, 100, 100, true, 120), sequence_bindings);
    CHECK_TRUE(result.swallow);
    CHECK_TRUE(result.action_count == 1);
    CHECK_TRUE(mgh::wheel_sequence_length(result.actions[0].wheel_sequence) == 2);
    session.reset();
    session.handle_event(session_event(mgh::SessionEventKind::right_down), sequence_bindings);
    session.handle_event(session_event(mgh::SessionEventKind::wheel, 100, 100, true, 120), sequence_bindings);
    result = session.handle_event(session_event(mgh::SessionEventKind::wheel, 100, 100, true, -120), sequence_bindings);
    CHECK_TRUE(!result.swallow);
    CHECK_TRUE(result.action_count == 1);
    CHECK_TRUE(result.actions[0].kind == mgh::SessionActionKind::unmatched_wheel);
}

void test_bounded_mouse_queue() {
    mgh::BoundedMouseQueue<2> queue;
    mgh::QueuedMouseEvent move;
    move.droppable = true;
    move.result.action_count = 1;
    move.result.actions[0].kind = mgh::SessionActionKind::move;
    mgh::QueuedMouseEvent critical;
    critical.result.action_count = 1;
    critical.result.actions[0].kind = mgh::SessionActionKind::begin;

    CHECK_TRUE(queue.push(move));
    move.result.actions[0].point.x = 2;
    CHECK_TRUE(queue.push(move));
    CHECK_TRUE(queue.push(critical));
    CHECK_TRUE(queue.dropped() == 1);
    CHECK_TRUE(queue.size() == 2);

    mgh::QueuedMouseEvent out;
    CHECK_TRUE(queue.pop(out));
    CHECK_TRUE(out.droppable);
    CHECK_TRUE(out.result.actions[0].point.x == 2);
    CHECK_TRUE(queue.push(critical));
    CHECK_TRUE(queue.dropped() == 1);
    CHECK_TRUE(!queue.push(critical));
    CHECK_TRUE(queue.dropped() == 2);
    CHECK_TRUE(!queue.push(move));
    CHECK_TRUE(queue.dropped() == 3);
    CHECK_TRUE(queue.pop(out));
    CHECK_TRUE(out.result.actions[0].kind == mgh::SessionActionKind::begin);
}

void test_hook_health_fake_clock() {
    mgh::HookHealth health;
    health.note_event(1000);
    CHECK_TRUE(!health.should_reinstall(3999, true));
    CHECK_TRUE(!health.should_reinstall(5000, false));
    CHECK_TRUE(health.should_reinstall(4000, true));

    health.note_reinstall(4000, true);
    CHECK_TRUE(health.reinstall_attempts() == 1);
    CHECK_TRUE(!health.should_reinstall(7000, true));
    CHECK_TRUE(health.should_reinstall(8000, true));
    health.note_event(8000);
    CHECK_TRUE(health.reinstall_attempts() == 0);

    CHECK_TRUE(health.should_reinstall(11000, true));
    health.note_reinstall(11000, false);
    CHECK_TRUE(!health.should_reinstall(11999, true));
    CHECK_TRUE(health.should_reinstall(12000, true));
    health.note_reinstall(12000, false);
    CHECK_TRUE(health.backoff_ms() == 2000);
    CHECK_TRUE(!health.should_reinstall(13999, true));
    CHECK_TRUE(health.should_reinstall(14000, true));
}

std::size_t benchmark_sink = 0;

void run_binding_benchmark() {
    constexpr std::size_t kLookups = 20000;
    for (const std::size_t count : { std::size_t{100}, std::size_t{1000} }) {
        mgh::GestureSettings settings;
        settings.bindings = make_benchmark_bindings(count);
        const mgh::BindingSnapshot snapshot(settings);
        std::vector<std::string> probes;
        probes.reserve(kLookups);
        for (std::size_t i = 0; i < kLookups; ++i) {
            probes.push_back(settings.bindings[i % count].gesture);
        }

        const auto index_begin = std::chrono::steady_clock::now();
        for (const auto& probe : probes) {
            benchmark_sink += snapshot.exact_matches(probe).size();
            benchmark_sink += snapshot.has_binding_prefix(probe) ? 1 : 0;
        }
        const auto index_end = std::chrono::steady_clock::now();

        const auto linear_begin = std::chrono::steady_clock::now();
        for (const auto& probe : probes) {
            for (const auto& binding : settings.bindings) {
                if (binding.gesture == probe) ++benchmark_sink;
                if (std::string_view(binding.gesture).substr(0, probe.size()) == probe) ++benchmark_sink;
            }
        }
        const auto linear_end = std::chrono::steady_clock::now();

        const double index_ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(index_end - index_begin).count()) / kLookups;
        const double linear_ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(linear_end - linear_begin).count()) / kLookups;
        std::cout << "Binding benchmark " << count << " rows: indexed " << index_ns
                  << " ns/lookup, linear " << linear_ns << " ns/lookup (informational)\n";
    }
    std::cout << "Benchmark sink: " << benchmark_sink << '\n';
}

} // namespace

int main(int argc, char** argv) {
    test_tokens();
    test_candidate_order();
    test_recognizer_trajectories();
    test_config_roundtrip_and_migration();
    test_config_corruption_and_duplicates();
    test_command_identity_resolution();
    test_binding_index_differential();
    test_gesture_list_sort_state_and_gesture();
    test_gesture_list_sort_command_place_and_ties();
    test_gesture_list_sort_view_and_selection();
    test_gesture_list_grouping();
    test_editor_command_state();
    test_editor_state_and_duplicates();
    test_editor_guard_decisions();
    test_session_idle_drag_and_reconcile();
    test_session_rocker();
    test_session_wheel_and_fractional_delta();
    test_bounded_mouse_queue();
    test_hook_health_fake_clock();

    if (failures != 0) {
        std::cerr << failures << " logic test assertion(s) failed\n";
        return 1;
    }
    std::cout << "Logic tests passed: 12 tokens, candidate order, "
              << trajectory_checks << " deterministic trajectories, config, index, sorting, session and health\n";
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        run_binding_benchmark();
    } else if (argc != 1) {
        std::cerr << "Usage: logic_tests [--benchmark]\n";
        return 2;
    }
    return 0;
}
