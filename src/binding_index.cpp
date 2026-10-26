#include "binding_index.h"

namespace mgh {
namespace {

constexpr std::size_t kMaxWheelTokens = 32;

} // namespace

WheelSequenceKey append_wheel_sequence(WheelSequenceKey key, bool up) {
    if (key == kInvalidWheelSequence) return key;
    const std::size_t length = wheel_sequence_length(key);
    if (length >= kMaxWheelTokens) return kInvalidWheelSequence;
    const std::uint32_t bits = static_cast<std::uint32_t>(key);
    const std::uint32_t next_bits = up ? bits : (bits | (std::uint32_t{1} << length));
    return (WheelSequenceKey{length + 1} << 32) | next_bits;
}

std::size_t wheel_sequence_length(WheelSequenceKey key) {
    if (key == kInvalidWheelSequence) return kMaxWheelTokens + 1;
    return static_cast<std::size_t>(key >> 32);
}

BindingSnapshot::BindingSnapshot(GestureSettings settings)
    : settings_(std::move(settings)) {
    sorted_gestures_.reserve(settings_.bindings.size());
    for (std::size_t i = 0; i < settings_.bindings.size(); ++i) {
        const auto& binding = settings_.bindings[i];
        exact_[binding.gesture].push_back(i);
        sorted_gestures_.push_back(binding.gesture);

        std::vector<Token> tokens;
        if (!parse_gesture(binding.gesture, tokens)) continue;
        WheelSequenceKey key = 0;
        for (std::size_t token_index = 0; token_index < tokens.size(); ++token_index) {
            const Token token = tokens[token_index];
            if (token != Token::wheel_up && token != Token::wheel_down) break;
            key = append_wheel_sequence(key, token == Token::wheel_up);
            if (key == kInvalidWheelSequence) break;
            auto& match = wheel_[key];
            match.prefix = true;
            if (token_index + 1 == tokens.size()) match.exact = true;
        }
    }
    std::sort(sorted_gestures_.begin(), sorted_gestures_.end());
}

bool BindingSnapshot::has_binding(std::string_view gesture) const {
    return exact_.find(gesture) != exact_.end();
}

bool BindingSnapshot::has_binding_prefix(std::string_view gesture) const {
    const auto it = std::lower_bound(
        sorted_gestures_.begin(), sorted_gestures_.end(), gesture,
        [](const std::string& value, std::string_view prefix) {
            return std::string_view(value) < prefix;
        });
    if (it == sorted_gestures_.end()) return false;
    const std::string_view value(*it);
    return value.size() >= gesture.size() && value.substr(0, gesture.size()) == gesture;
}

std::span<const std::size_t> BindingSnapshot::exact_matches(std::string_view gesture) const {
    const auto it = exact_.find(gesture);
    if (it == exact_.end()) return {};
    return it->second;
}

WheelMatch BindingSnapshot::wheel_match(WheelSequenceKey key) const {
    const auto it = wheel_.find(key);
    return it != wheel_.end() ? it->second : WheelMatch{};
}

} // namespace mgh
