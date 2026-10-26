#include "binding_index.h"
#include "command_identity.h"
#include "gesture_config.h"
#include "gesture_recognizer.h"
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

    if (failures != 0) {
        std::cerr << failures << " logic test assertion(s) failed\n";
        return 1;
    }
    std::cout << "Logic tests passed: 12 tokens, candidate order, "
              << trajectory_checks << " deterministic trajectories, config and binding index\n";
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        run_binding_benchmark();
    } else if (argc != 1) {
        std::cerr << "Usage: logic_tests [--benchmark]\n";
        return 2;
    }
    return 0;
}
