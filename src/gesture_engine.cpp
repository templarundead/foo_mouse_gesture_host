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

    replay_guard_ = GetTickCount64();

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
    replay_pending_ = false;
    reset_session();
}

void GestureEngine::reload() {
    snapshot_ = std::make_shared<BindingSnapshot>(GestureConfig::instance().load_from_storage());
    recognizer_.set_stroke_length(snapshot_->settings().stroke_length);
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
    wheel_accum_ = 0;
    recognizer_.cancel();
}

bool GestureEngine::has_binding(const std::string& gesture) const {
    return snapshot_->has_binding(gesture);
}

bool GestureEngine::has_binding_prefix(const std::string& gesture) const {
    return snapshot_->has_binding_prefix(gesture);
}

void GestureEngine::queue_gesture(const std::string& gesture) {
    if (sink_ == nullptr) return;
    pending_.push_back(gesture);
    PostMessageW(sink_, kMsgExecuteGesture, 0, 0);
}

void GestureEngine::request_right_click_replay() {
    replay_pending_ = true;
    if (sink_ != nullptr) {
        PostMessageW(sink_, kMsgExecuteGesture, 0, 0);
    } else {
        // Окно-приёмник ещё не создано - отправляем напрямую: лучше так,
        // чем потерять клик совсем.
        replay_pending_ = false;
        replay_right_click();
    }
}

bool GestureEngine::handle_wheel_notch(bool up) {
    const std::string token = token_id(up ? Token::wheel_up : Token::wheel_down);
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

    // Накопленная последовательность не подошла. Пробуем начать заново
    // с этого щелчка, иначе один лишний щелчок "залипал" и все следующие
    // жесты колесом не срабатывали.
    wheel_sequence_.clear();
    if (has_binding(token)) {
        wheel_used_ = true;
        queue_gesture(token);
        return true;
    }

    report_unmatched_gesture(token);
    return false;
}

void GestureEngine::report_unmatched_gesture(const std::string& gesture) {
    for (const auto& seen : reported_gestures_) {
        if (seen == gesture) return;
    }
    if (reported_gestures_.size() >= 32) reported_gestures_.erase(reported_gestures_.begin());
    reported_gestures_.push_back(gesture);

    FB2K_console_formatter() << "Mouse Gesture Host: gesture \"" << gesture.c_str()
                             << "\" has no binding";
}

void GestureEngine::drain_queue() {
    if (replay_pending_) {
        replay_pending_ = false;
        replay_right_click();
    }

    while (!pending_.empty()) {
        const std::string gesture = pending_.front();
        pending_.pop_front();

        // Snapshot остаётся неизменным, даже если команда откроет настройки и
        // вызовет reload: индексы и порядок строк внутри этого прохода согласованы.
        const BindingSnapshotPtr snapshot = snapshot_;
        const auto matches = snapshot->exact_matches(gesture);
        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (i > 0 && snapshot->settings().first_only) break;

            const GestureBinding& binding = snapshot->settings().bindings[matches[i]];
            if (!execute_binding(binding)) {
                // Привязка есть, но команда не найдена (меню переименовали,
                // компонент не загрузился) - иначе это выглядит как
                // "жест не работает".
                FB2K_console_formatter()
                    << "Mouse Gesture Host: gesture \"" << gesture.c_str()
                    << "\" matched, but command \"" << binding.command_path.c_str()
                    << "\" was not found";
            }
        }
    }
}

bool GestureEngine::handle(WPARAM msg, const MSLLHOOKSTRUCT& info) {
    if (snapshot_->settings().disabled) return false;

    // Свои же синтезированные события не перехватываем - иначе правый клик
    // никогда не дойдёт до foobar2000. Игнорируем их только короткое время
    // после SendInput: тачпады и часть мышей тоже помечают ввод как
    // LLMHF_INJECTED, и постоянный фильтр отключил бы жесты для них.
    if ((info.flags & LLMHF_INJECTED) != 0 && replay_guard_ != 0 &&
        GetTickCount64() - replay_guard_ < 500) {
        return false;
    }

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
        wheel_accum_ = 0;

        recognizer_.set_stroke_length(snapshot_->settings().stroke_length);
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
            // Завершение рокера "L+R". Нажатие мы уже съели, поэтому съедаем
            // и отпускание - иначе хост получил бы "up без down".
            if (rocker_armed_) {
                rocker_armed_ = false;
                queue_gesture(token_id(Token::rocker_left_right));
            }
            return true;
        }

        bool swallow = rocker_armed_ || wheel_used_;
        rocker_armed_ = false;

        const bool wheel_in_use = wheel_used_;

        // Сохраняем последний штрих, который оказался чуть короче основного
        // порога: раньше он просто терялся при отпускании ПКМ.
        recognizer_.finish();
        const std::string gesture = recognizer_.canonical();
        const auto match_candidates = recognizer_.match_candidates();
        recognizer_.cancel();

        bool executed = false;
        for (const auto& candidate : match_candidates) {
            if (!candidate.empty() && has_binding(candidate)) {
                queue_gesture(candidate);
                executed = true;
                swallow = true;
                break;
            }
        }
        if (!executed && !gesture.empty()) report_unmatched_gesture(gesture);

        // ПКМ была подавлена. Если это был обычный щелчок (без сдвига), а жест
        // не сработал - возвращаем foobar2000 правый клик, чтобы открылось
        // контекстное меню. Отпускание кнопки при этом всегда съедаем сами:
        // иначе хост получил бы "up без down".
        const bool was_suppressed = suppress_right_;
        const bool was_tap = !right_moved_;
        suppress_right_ = false;
        right_moved_ = false;

        if (was_suppressed) {
            swallow = true;
            // Сам SendInput - только из цикла сообщений: изнутри хука он
            // ждёт LowLevelHooksTimeout и меню открывается с задержкой.
            if (!executed && !wheel_in_use && was_tap) request_right_click_replay();
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

            // ВАЖНО: WM_MOUSEMOVE нельзя съедать. Возврат true из
            // WH_MOUSE_LL для движения блокирует само перемещение курсора
            // (курсор "замерзает"). Драг у хоста уже не начнётся, потому что
            // мы съели WM_RBUTTONDOWN, - этого достаточно.
        }
        return false;
    }

    case WM_MOUSEWHEEL: {
        if (!right_down_ || !session_from_right_ || rocker_armed_) return false;
        // Колесо вне окна foobar2000 не должно превращаться в жест.
        if (!over_main_window(pt)) return false;

        const int delta = static_cast<short>(HIWORD(info.mouseData));
        if (delta == 0) return false;

        // Копим дельту: у высокоразрешённых колёс и тачпадов один щелчок
        // приходит как несколько событий по 1..30 единиц.
        wheel_accum_ += delta;
        int notches = wheel_accum_ / WHEEL_DELTA;
        if (notches == 0) return false;
        wheel_accum_ -= notches * WHEEL_DELTA;

        int count = notches > 0 ? notches : -notches;
        if (count > 4) count = 4;

        bool handled = false;
        for (int i = 0; i < count; ++i) {
            if (handle_wheel_notch(notches > 0)) handled = true;
        }
        return handled;
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
