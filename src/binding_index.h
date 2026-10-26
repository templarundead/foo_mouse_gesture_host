#pragma once

#include "gesture_config.h"

#include <span>
#include <unordered_map>

namespace mgh {

//! Числовой ключ последовательности wheel-токенов для callback без строк.
//! Младшие 32 бита - направления (0=up, 1=down), старшие - длина (0..32).
using WheelSequenceKey = std::uint64_t;
constexpr WheelSequenceKey kInvalidWheelSequence = ~WheelSequenceKey{};

WheelSequenceKey append_wheel_sequence(WheelSequenceKey key, bool up);
std::size_t wheel_sequence_length(WheelSequenceKey key);

struct WheelMatch {
    bool exact = false;
    bool prefix = false;
};

//! Неизменяемая конфигурация и построенные один раз индексы.
//! Hot path получает span номеров строк и никогда не копирует сами bindings.
class BindingSnapshot {
public:
    explicit BindingSnapshot(GestureSettings settings);

    const GestureSettings& settings() const { return settings_; }

    bool has_binding(std::string_view gesture) const;
    bool has_binding_prefix(std::string_view gesture) const;
    std::span<const std::size_t> exact_matches(std::string_view gesture) const;

    WheelMatch wheel_match(WheelSequenceKey key) const;

private:
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const {
            return std::hash<std::string_view>{}(value);
        }
    };
    struct StringEqual {
        using is_transparent = void;
        bool operator()(std::string_view left, std::string_view right) const {
            return left == right;
        }
    };

    using MatchMap = std::unordered_map<std::string, std::vector<std::size_t>,
                                        StringHash, StringEqual>;

    GestureSettings settings_;
    MatchMap exact_;
    std::vector<std::string> sorted_gestures_;
    std::unordered_map<WheelSequenceKey, WheelMatch> wheel_;
};

using BindingSnapshotPtr = std::shared_ptr<const BindingSnapshot>;

} // namespace mgh
