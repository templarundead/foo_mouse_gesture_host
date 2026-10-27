#include "gesture_session.h"

namespace mgh {
namespace {

constexpr int kDragThreshold = 3;
constexpr int kWheelDelta = 120;
constexpr int kMaxWheelNotchesPerEvent = 4;
constexpr std::uint64_t kHookSilenceTimeoutMs = 3000;
constexpr std::uint32_t kInitialBackoffMs = 1000;
constexpr std::uint32_t kMaximumBackoffMs = 30000;

void add_action(SessionResult& result, const SessionAction& action) {
    if (result.action_count < result.actions.size()) {
        result.actions[result.action_count++] = action;
    }
}

} // namespace

SessionResult GestureSession::handle_event(const SessionEvent& event,
                                           const BindingSnapshot& bindings) {
    SessionResult result;

    switch (event.kind) {
    case SessionEventKind::right_down: {
        if (left_down_ && event.over_main_window && bindings.has_binding("L+R")) {
            right_down_ = true;
            session_from_right_ = false;
            rocker_armed_ = true;
            result.swallow = true;
            return result;
        }
        if (!event.over_main_window) return result;

        right_down_ = true;
        session_from_right_ = true;
        rocker_armed_ = false;
        wheel_used_ = false;
        wheel_sequence_ = 0;
        suppress_right_ = true;
        right_down_point_ = event.point;
        right_moved_ = false;
        wheel_accum_ = 0;

        SessionAction action;
        action.kind = SessionActionKind::begin;
        action.point = event.point;
        add_action(result, action);
        result.swallow = true;
        return result;
    }

    case SessionEventKind::left_down: {
        if (!event.over_main_window) return result;
        left_down_ = true;
        if (right_down_ && session_from_right_ && !rocker_armed_ &&
            bindings.has_binding("R+L")) {
            rocker_armed_ = true;
            result.swallow = true;
        }
        return result;
    }

    case SessionEventKind::left_up: {
        const bool was_down = left_down_;
        left_down_ = false;
        if (was_down && rocker_armed_ && right_down_ && session_from_right_) {
            rocker_armed_ = false;
            SessionAction action;
            action.kind = SessionActionKind::emit_rocker;
            action.rocker_token = Token::rocker_right_left;
            add_action(result, action);
            result.swallow = true;
        }
        return result;
    }

    case SessionEventKind::right_up: {
        const bool was_down = right_down_;
        right_down_ = false;
        if (!was_down) return result;

        if (!session_from_right_) {
            if (rocker_armed_) {
                rocker_armed_ = false;
                SessionAction action;
                action.kind = SessionActionKind::emit_rocker;
                action.rocker_token = Token::rocker_left_right;
                add_action(result, action);
            }
            result.swallow = true;
            return result;
        }

        result.swallow = rocker_armed_ || wheel_used_ || suppress_right_;
        rocker_armed_ = false;

        SessionAction action;
        action.kind = SessionActionKind::finish;
        action.point = event.point;
        action.right_down_point = right_down_point_;
        action.was_tap = !right_moved_;
        action.wheel_used = wheel_used_;
        action.was_suppressed = suppress_right_;
        add_action(result, action);

        suppress_right_ = false;
        right_moved_ = false;
        wheel_used_ = false;
        wheel_sequence_ = 0;
        return result;
    }

    case SessionEventKind::mouse_move: {
        if (tracks_mouse_move()) {
            SessionAction action;
            action.kind = SessionActionKind::move;
            action.point = event.point;
            add_action(result, action);

            const long dx = event.point.x - right_down_point_.x;
            const long dy = event.point.y - right_down_point_.y;
            if (dx > kDragThreshold || dx < -kDragThreshold ||
                dy > kDragThreshold || dy < -kDragThreshold) {
                right_moved_ = true;
            }
        }
        // WM_MOUSEMOVE никогда не подавляется: иначе курсор перестаёт двигаться.
        return result;
    }

    case SessionEventKind::wheel:
        return handle_wheel(event, bindings);
    }
    return result;
}

SessionResult GestureSession::handle_wheel(const SessionEvent& event,
                                           const BindingSnapshot& bindings) {
    SessionResult result;
    if (!right_down_ || !session_from_right_ || rocker_armed_ ||
        !event.over_main_window || event.wheel_delta == 0) {
        return result;
    }

    wheel_accum_ += event.wheel_delta;
    const int notches = wheel_accum_ / kWheelDelta;
    if (notches == 0) return result;
    wheel_accum_ -= notches * kWheelDelta;

    int count = notches > 0 ? notches : -notches;
    if (count > kMaxWheelNotchesPerEvent) count = kMaxWheelNotchesPerEvent;
    for (int i = 0; i < count; ++i) {
        SessionResult notch = handle_wheel_notch(notches > 0, bindings);
        result.swallow = result.swallow || notch.swallow;
        for (std::size_t action = 0; action < notch.action_count; ++action) {
            add_action(result, notch.actions[action]);
        }
    }
    return result;
}

SessionResult GestureSession::handle_wheel_notch(bool up, const BindingSnapshot& bindings) {
    SessionResult result;
    const WheelSequenceKey next = append_wheel_sequence(wheel_sequence_, up);
    const WheelMatch match = bindings.wheel_match(next);

    if (match.exact) {
        wheel_used_ = true;
        wheel_sequence_ = 0;
        SessionAction action;
        action.kind = SessionActionKind::emit_wheel;
        action.wheel_sequence = next;
        add_action(result, action);
        result.swallow = true;
        return result;
    }
    if (match.prefix) {
        wheel_used_ = true;
        wheel_sequence_ = next;
        result.swallow = true;
        return result;
    }

    // Сохраняем старое правило: после неудачной цепочки один notch проверяется
    // только на exact, а не остаётся висеть новым prefix.
    wheel_sequence_ = 0;
    const WheelSequenceKey single = append_wheel_sequence(0, up);
    if (bindings.wheel_match(single).exact) {
        wheel_used_ = true;
        SessionAction action;
        action.kind = SessionActionKind::emit_wheel;
        action.wheel_sequence = single;
        add_action(result, action);
        result.swallow = true;
        return result;
    }

    SessionAction action;
    action.kind = SessionActionKind::unmatched_wheel;
    action.wheel_sequence = single;
    add_action(result, action);
    return result;
}

SessionResult GestureSession::reconcile_left_button(bool left_down,
                                                    const BindingSnapshot& bindings) {
    // right_up приходит только из hook: таймер не должен открывать меню во время удержания.
    if (left_down_ && !left_down) {
        SessionEvent event;
        event.kind = SessionEventKind::left_up;
        event.point = right_down_point_;
        return handle_event(event, bindings);
    }
    return {};
}

void GestureSession::reset() {
    suppress_right_ = false;
    right_down_point_ = {};
    right_moved_ = false;
    right_down_ = false;
    left_down_ = false;
    session_from_right_ = true;
    rocker_armed_ = false;
    wheel_used_ = false;
    wheel_sequence_ = 0;
    wheel_accum_ = 0;
}

GestureSessionState GestureSession::state() const {
    if (rocker_armed_) {
        return session_from_right_ ? GestureSessionState::rocker_right_left
                                   : GestureSessionState::rocker_left_right;
    }
    if (right_down_ && session_from_right_) {
        return wheel_used_ ? GestureSessionState::wheel : GestureSessionState::right_down;
    }
    return GestureSessionState::idle;
}

void HookHealth::note_event(std::uint64_t now_ms) {
    last_event_ms_ = now_ms;
    retry_after_ms_ = now_ms;
    attempts_ = 0;
}

bool HookHealth::should_reinstall(std::uint64_t now_ms, bool input_activity) const {
    return input_activity && now_ms >= retry_after_ms_ &&
           now_ms - last_event_ms_ >= kHookSilenceTimeoutMs;
}

void HookHealth::note_reinstall(std::uint64_t now_ms, bool success) {
    if (success) last_event_ms_ = now_ms;
    attempts_ = (std::min)(attempts_ + 1, 6u);
    retry_after_ms_ = now_ms + backoff_ms() + (success ? kHookSilenceTimeoutMs : 0);
}

std::uint32_t HookHealth::backoff_ms() const {
    if (attempts_ == 0) return kInitialBackoffMs;
    const unsigned shift = (std::min)(attempts_ - 1, 5u);
    const std::uint32_t value = kInitialBackoffMs << shift;
    return (std::min)(value, kMaximumBackoffMs);
}

} // namespace mgh
