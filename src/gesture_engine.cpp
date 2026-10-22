#include "gesture_engine.h"

#include "command_catalog.h"

namespace mgh {
namespace {

constexpr wchar_t kSinkClass[] = L"foo_mouse_gesture_host_sink";
constexpr UINT kMsgExecuteGesture = WM_APP + 0x4D47; // 'MG'

bool over_main_window(POINT pt) {
    const HWND main_window = core_api::get_main_window();
    if (main_window == nullptr) return false;

    const HWND window = WindowFromPoint(pt);
    if (window == nullptr) return false;

    return GetAncestor(window, GA_ROOT) == main_window;
}

void register_sink_class() {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &GestureEngine::sink_proc;
    wc.hInstance = core_api::get_my_instance();
    wc.lpszClassName = kSinkClass;
    RegisterClassExW(&wc);

    registered = true;
}

//! Сдвиг в пикселях, после которого считаем, что пользователь начал драг.
constexpr int kDragThreshold = 3;

} // namespace

void GestureEngine::replay_right_click() {
    // Синтетические события помечаются LLMHF_INJECTED и игнорируются хуком,
    // поэтому рекурсии нет, а foobar2000 получает обычный правый клик.
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;

    if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
        FB2K_console_formatter() << "Mouse Gesture Host: failed to replay the right click ("
                                 << static_cast<unsigned long>(GetLastError()) << ")";
    }
}

GestureEngine& GestureEngine::instance() {
    static GestureEngine engine;
    return engine;
}

void GestureEngine::install() {
    if (hook_ != nullptr) return;

    reload();

    register_sink_class();
    if (sink_ == nullptr) {
        sink_ = CreateWindowExW(0, kSinkClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, core_api::get_my_instance(), nullptr);
    }

    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &GestureEngine::mouse_hook_proc,
                              core_api::get_my_instance(), 0);
    if (hook_ == nullptr) {
        FB2K_console_formatter() << "Mouse Gesture Host: failed to install the mouse hook ("
                                 << static_cast<unsigned long>(GetLastError()) << ")";
    }
}

void GestureEngine::uninstall() {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (sink_ != nullptr) {
        DestroyWindow(sink_);
        sink_ = nullptr;
    }
    pending_.clear();
    reset_session();
}

void GestureEngine::reload() {
    settings_ = GestureConfig::instance().load_from_storage();
    recognizer_.set_stroke_length(settings_.stroke_length);
}

void GestureEngine::reset_session() {
    suppress_right_ = false;
    right_moved_ = false;
    right_down_ = false;
    left_down_ = false;
    session_from_right_ = true;
    rocker_armed_ = false;
    wheel_used_ = false;
    wheel_sequence_.clear();
    recognizer_.cancel();
}

bool GestureEngine::has_binding(const std::string& gesture) const {
    for (const auto& binding : settings_.bindings) {
        if (binding.gesture == gesture) return true;
    }
    return false;
}

bool GestureEngine::has_binding_prefix(const std::string& gesture) const {
    for (const auto& binding : settings_.bindings) {
        if (binding.gesture.compare(0, gesture.size(), gesture) == 0) return true;
    }
    return false;
}

void GestureEngine::queue_gesture(const std::string& gesture) {
    if (sink_ == nullptr) return;
    pending_.push_back(gesture);
    PostMessageW(sink_, kMsgExecuteGesture, 0, 0);
}

void GestureEngine::drain_queue() {
    while (!pending_.empty()) {
        const std::string gesture = pending_.front();
        pending_.pop_front();

        // Копируем совпадения: команда может открыть настройки и изменить таблицу
        // привязок, пока мы по ней идём.
        std::vector<GestureBinding> matches;
        for (const auto& binding : settings_.bindings) {
            if (binding.gesture == gesture) matches.push_back(binding);
        }

        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (i > 0 && settings_.first_only) break;
            execute_binding(matches[i]);
        }
    }
}

bool GestureEngine::handle(WPARAM msg, const MSLLHOOKSTRUCT& info) {
    if (settings_.disabled) return false;

    // Свои же синтезированные события не перехватываем - иначе правый клик
    // никогда не дойдёт до foobar2000.
    if ((info.flags & LLMHF_INJECTED) != 0) return false;

    const POINT pt = info.pt;

    switch (msg) {
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK: {
        // Рокер "L+R": ЛКМ уже зажата, кликаем ПКМ.
        if (left_down_ && over_main_window(pt) &&
            has_binding(token_id(Token::rocker_left_right))) {
            right_down_ = true;
            session_from_right_ = false;
            rocker_armed_ = true;
            return true;
        }

        if (!over_main_window(pt)) return false;

        right_down_ = true;
        session_from_right_ = true;
        rocker_armed_ = false;
        wheel_used_ = false;
        wheel_sequence_.clear();

        // Событие не отдаём хосту: иначе foobar2000 сразу начнёт своё
        // перетаскивание треков правой кнопкой. Если жест не распознается,
        // на отпускании отправим обычный правый клик.
        suppress_right_ = true;
        right_down_point_ = pt;
        right_moved_ = false;

        recognizer_.set_stroke_length(settings_.stroke_length);
        recognizer_.begin(pt.x, pt.y);
        return true;
    }

    case WM_LBUTTONDOWN: {
        if (!over_main_window(pt)) return false;
        left_down_ = true;

        // Рокер "R+L": ПКМ уже зажата, кликаем ЛКМ.
        if (right_down_ && session_from_right_ && !rocker_armed_ &&
            has_binding(token_id(Token::rocker_right_left))) {
            rocker_armed_ = true;
            return true;
        }
        return false;
    }

    case WM_LBUTTONUP: {
        const bool was_down = left_down_;
        left_down_ = false;
        if (was_down && rocker_armed_ && right_down_ && session_from_right_) {
            rocker_armed_ = false;
            queue_gesture(token_id(Token::rocker_right_left));
            return true;
        }
        return false;
    }

    case WM_RBUTTONUP: {
        const bool was_down = right_down_;
        right_down_ = false;

        if (!was_down) return false;

        if (!session_from_right_) {
            // Завершение рокера "L+R".
            if (rocker_armed_) {
                rocker_armed_ = false;
                queue_gesture(token_id(Token::rocker_left_right));
                return true;
            }
            return false;
        }

        bool swallow = rocker_armed_ || wheel_used_;
        rocker_armed_ = false;

        const bool wheel_in_use = wheel_used_;

        const std::string gesture = recognizer_.canonical();
        recognizer_.cancel();

        bool executed = false;
        if (!gesture.empty() && has_binding(gesture)) {
            queue_gesture(gesture);
            executed = true;
            swallow = true;
        }

        // ПКМ была подавлена: если жест не сработал, возвращаем foobar2000
        // обычный правый клик, чтобы открылось контекстное меню.
        const bool was_suppressed = suppress_right_;
        suppress_right_ = false;
        right_moved_ = false;

        if (was_suppressed && !executed && !wheel_in_use) {
            replay_right_click();
        }

        wheel_used_ = false;
        wheel_sequence_.clear();
        return swallow;
    }

    case WM_MOUSEMOVE: {
        if (right_down_ && session_from_right_ && !rocker_armed_) {
            recognizer_.move(pt.x, pt.y);

            if (!right_moved_) {
                const int dx = pt.x - right_down_point_.x;
                const int dy = pt.y - right_down_point_.y;
                if (dx > kDragThreshold || dx < -kDragThreshold ||
                    dy > kDragThreshold || dy < -kDragThreshold) {
                    right_moved_ = true;
                }
            }

            // Пока идёт жест, движения тоже не отдаём хосту - иначе он
            // успеет начать перетаскивание треков.
            if (suppress_right_) return true;
        }
        return false;
    }

    case WM_MOUSEWHEEL: {
        if (!right_down_ || !session_from_right_ || rocker_armed_) return false;
        // Колесо вне окна foobar2000 не должно превращаться в жест.
        if (!over_main_window(pt)) return false;

        const int delta = static_cast<short>(HIWORD(info.mouseData));
        if (delta == 0) return false;

        const std::string token = token_id(delta > 0 ? Token::wheel_up : Token::wheel_down);
        const std::string next = wheel_sequence_.empty() ? token : wheel_sequence_ + " " + token;

        // Точное совпадение - исполняем и начинаем последовательность заново.
        if (has_binding(next)) {
            wheel_used_ = true;
            wheel_sequence_.clear();
            queue_gesture(next);
            return true;
        }

        // Есть более длинный жест с таким префиксом - копим дальше.
        if (has_binding_prefix(next)) {
            wheel_used_ = true;
            wheel_sequence_ = next;
            return true;
        }

        wheel_sequence_.clear();
        return false;
    }

    default:
        return false;
    }
}

LRESULT CALLBACK GestureEngine::mouse_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
        if (info != nullptr && GestureEngine::instance().handle(wparam, *info)) {
            // Съедаем событие: оно не должно дойти ни до foobar2000, ни до других приложений.
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK GestureEngine::sink_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == kMsgExecuteGesture) {
        GestureEngine::instance().drain_queue();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace mgh
