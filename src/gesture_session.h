#pragma once

#include "binding_index.h"

#include <array>

namespace mgh {

enum class GestureSessionState {
    idle,
    right_down,
    rocker_right_left,
    rocker_left_right,
    wheel,
};

enum class SessionEventKind {
    right_down,
    left_down,
    left_up,
    right_up,
    mouse_move,
    wheel,
};

struct SessionPoint {
    long x = 0;
    long y = 0;
};

struct SessionEvent {
    SessionEventKind kind = SessionEventKind::mouse_move;
    SessionPoint point;
    int wheel_delta = 0;
    bool over_main_window = false;
};

enum class SessionActionKind {
    begin,
    move,
    finish,
    emit_rocker,
    emit_wheel,
    unmatched_wheel,
};

struct SessionAction {
    SessionActionKind kind = SessionActionKind::begin;
    SessionPoint point;
    SessionPoint right_down_point;
    Token rocker_token = Token::rocker_right_left;
    WheelSequenceKey wheel_sequence = 0;
    bool was_tap = false;
    bool wheel_used = false;
    bool was_suppressed = false;
};

struct SessionResult {
    bool swallow = false;
    std::size_t action_count = 0;
    std::array<SessionAction, 4> actions;
};

//! Числовой автомат подавления, вызываемый из WH_MOUSE_LL.
//! Не создаёт строк и не занимается распознаванием траектории.
class GestureSession {
public:
    SessionResult handle_event(const SessionEvent& event, const BindingSnapshot& bindings);
    SessionResult reconcile_buttons(bool left_down, bool right_down,
                                    const BindingSnapshot& bindings);
    void reset();

    GestureSessionState state() const;
    bool right_moved() const { return right_moved_; }
    int wheel_accumulator() const { return wheel_accum_; }

private:
    SessionResult handle_wheel(const SessionEvent& event, const BindingSnapshot& bindings);
    SessionResult handle_wheel_notch(bool up, const BindingSnapshot& bindings);

    bool suppress_right_ = false;
    SessionPoint right_down_point_;
    bool right_moved_ = false;
    bool right_down_ = false;
    bool left_down_ = false;
    bool session_from_right_ = true;
    bool rocker_armed_ = false;
    bool wheel_used_ = false;
    WheelSequenceKey wheel_sequence_ = 0;
    int wheel_accum_ = 0;
};

struct QueuedMouseEvent {
    SessionResult result;
    bool droppable = false;
};

//! Fixed-capacity ring: никаких аллокаций в callback. Критическое событие
//! может вытеснить самое старое движение; остальные потери fail-open для нового события.
template<std::size_t Capacity = 256>
class BoundedMouseQueue {
    static_assert(Capacity > 0);

public:
    bool push(const QueuedMouseEvent& event) {
        if (size_ == Capacity) {
            if (event.droppable || !drop_oldest_move()) {
                ++dropped_;
                return false;
            }
            ++dropped_; // вытесненный старый move
        }
        items_[(head_ + size_) % Capacity] = event;
        ++size_;
        return true;
    }

    bool pop(QueuedMouseEvent& out) {
        if (size_ == 0) return false;
        out = std::move(items_[head_]);
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }

    void clear() {
        head_ = 0;
        size_ = 0;
    }

    std::size_t size() const { return size_; }
    std::uint64_t dropped() const { return dropped_; }

private:
    bool drop_oldest_move() {
        for (std::size_t offset = 0; offset < size_; ++offset) {
            if (!items_[(head_ + offset) % Capacity].droppable) continue;
            for (std::size_t i = offset; i + 1 < size_; ++i) {
                items_[(head_ + i) % Capacity] = std::move(items_[(head_ + i + 1) % Capacity]);
            }
            --size_;
            return true;
        }
        return false;
    }

    std::array<QueuedMouseEvent, Capacity> items_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t dropped_ = 0;
};

//! Health-логика с явными часами; Win32 adapter передаёт GetTickCount64.
class HookHealth {
public:
    void note_event(std::uint64_t now_ms);
    bool should_reinstall(std::uint64_t now_ms, bool input_activity) const;
    void note_reinstall(std::uint64_t now_ms, bool success);

    std::uint32_t backoff_ms() const;
    unsigned reinstall_attempts() const { return attempts_; }

private:
    std::uint64_t last_event_ms_ = 0;
    std::uint64_t retry_after_ms_ = 0;
    unsigned attempts_ = 0;
};

} // namespace mgh
