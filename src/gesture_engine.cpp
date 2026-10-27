#include "gesture_engine.h"

#include "command_catalog.h"

#include <process.h>

namespace mgh {
namespace {

constexpr wchar_t kSinkClass[] = L"foo_mouse_gesture_host_sink";
constexpr UINT kMsgExecuteGesture = WM_APP + 0x47;
constexpr UINT kMsgProcessMouse = WM_APP + 0x48;
constexpr UINT kMsgReload = WM_APP + 0x49;
constexpr UINT kMsgShutdown = WM_APP + 0x4A;
constexpr UINT_PTR kHealthTimer = 1;
constexpr UINT kHealthTimerMs = 1000;
constexpr std::size_t kMaxPendingReplays = 64;
constexpr std::size_t kMaxPendingCommands = 64;
constexpr std::size_t kMaxPendingLogs = 32;
constexpr std::uint64_t kSlowReplayLogMs = 30;
constexpr ULONG_PTR kOwnMouseMarker = static_cast<ULONG_PTR>(0x4D474853); // 'MGHS'

struct ReloadMessage {
    BindingSnapshotPtr snapshot;
};

bool over_main_window(HWND main_window, POINT pt) {
    if (main_window == nullptr) return false;
    const HWND window = WindowFromPoint(pt);
    if (window == nullptr) return false;
    return GetAncestor(window, GA_ROOT) == main_window;
}

void register_sink_class(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &GestureEngine::sink_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kSinkClass;
    RegisterClassExW(&wc);
    registered = true;
}

bool make_session_event(WPARAM msg, const MSLLHOOKSTRUCT& info, HWND main_window,
                        SessionEvent& event) {
    event.point = { info.pt.x, info.pt.y };
    switch (msg) {
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        event.kind = SessionEventKind::right_down;
        event.over_main_window = over_main_window(main_window, info.pt);
        return true;
    case WM_LBUTTONDOWN:
        event.kind = SessionEventKind::left_down;
        event.over_main_window = over_main_window(main_window, info.pt);
        return true;
    case WM_LBUTTONUP:
        event.kind = SessionEventKind::left_up;
        return true;
    case WM_RBUTTONUP:
        event.kind = SessionEventKind::right_up;
        return true;
    case WM_MOUSEMOVE:
        event.kind = SessionEventKind::mouse_move;
        return true;
    case WM_MOUSEWHEEL:
        event.kind = SessionEventKind::wheel;
        event.wheel_delta = static_cast<short>(HIWORD(info.mouseData));
        event.over_main_window = over_main_window(main_window, info.pt);
        return true;
    default:
        return false;
    }
}

std::string wheel_sequence_gesture(WheelSequenceKey key) {
    const std::size_t length = wheel_sequence_length(key);
    if (key == kInvalidWheelSequence || length == 0 || length > 32) return {};
    std::vector<Token> tokens;
    tokens.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        const bool down = (static_cast<std::uint32_t>(key) & (std::uint32_t{1} << i)) != 0;
        tokens.push_back(down ? Token::wheel_down : Token::wheel_up);
    }
    return join_tokens(tokens);
}

} // namespace

GestureEngine& GestureEngine::instance() {
    static GestureEngine engine;
    return engine;
}

void GestureEngine::install() {
    if (thread_ != nullptr) return;

    snapshot_ = std::make_shared<BindingSnapshot>(GestureConfig::instance().load_from_storage());
    worker_snapshot_ = snapshot_;
    instance_ = core_api::get_my_instance();
    main_window_ = core_api::get_main_window();
    register_sink_class(instance_);

    main_sink_ = CreateWindowExW(0, kSinkClass, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, instance_, nullptr);
    if (main_sink_ == nullptr) {
        FB2K_console_formatter() << "Mouse Gesture Host: failed to create the main-thread sink ("
                                 << static_cast<unsigned long>(GetLastError()) << ")";
        return;
    }

    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ready_event_ == nullptr) {
        FB2K_console_formatter() << "Mouse Gesture Host: failed to create the hook startup event ("
                                 << static_cast<unsigned long>(GetLastError()) << ")";
        DestroyWindow(main_sink_);
        main_sink_ = nullptr;
        return;
    }

    worker_error_ = 0;
    worker_ready_ = false;
    const uintptr_t handle = _beginthreadex(nullptr, 0, &GestureEngine::thread_proc, this, 0, nullptr);
    if (handle == 0) {
        FB2K_console_formatter() << "Mouse Gesture Host: failed to start the mouse-hook thread ("
                                 << static_cast<unsigned long>(GetLastError()) << ")";
        CloseHandle(ready_event_);
        ready_event_ = nullptr;
        DestroyWindow(main_sink_);
        main_sink_ = nullptr;
        return;
    }
    thread_ = reinterpret_cast<HANDLE>(handle);

    WaitForSingleObject(ready_event_, INFINITE);
    CloseHandle(ready_event_);
    ready_event_ = nullptr;

    if (!worker_ready_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
        DestroyWindow(main_sink_);
        main_sink_ = nullptr;
        FB2K_console_formatter() << "Mouse Gesture Host: mouse-hook thread initialization failed ("
                                 << static_cast<unsigned long>(worker_error_) << ")";
    }
}

void GestureEngine::uninstall() {
    if (thread_ != nullptr) {
        if (worker_sink_ != nullptr) {
            if (!PostMessageW(worker_sink_, kMsgShutdown, 0, 0)) {
                PostThreadMessageW(worker_thread_id_, WM_QUIT, 0, 0);
            }
        } else {
            PostThreadMessageW(worker_thread_id_, WM_QUIT, 0, 0);
        }
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (main_sink_ != nullptr) {
        DestroyWindow(main_sink_);
        main_sink_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(main_queue_mutex_);
    pending_replays_.clear();
    pending_commands_.clear();
    pending_logs_.clear();
    main_queue_posted_ = false;
    dropped_replays_ = 0;
    dropped_commands_ = 0;
    dropped_logs_ = 0;
}

void GestureEngine::reload() {
    BindingSnapshotPtr snapshot = std::make_shared<BindingSnapshot>(
        GestureConfig::instance().load_from_storage());
    snapshot_ = snapshot;

    if (thread_ == nullptr || worker_sink_ == nullptr) return;
    auto* message = new ReloadMessage{ std::move(snapshot) };
    if (!PostMessageW(worker_sink_, kMsgReload, 0, reinterpret_cast<LPARAM>(message))) {
        delete message;
        FB2K_console_formatter() << "Mouse Gesture Host: failed to post configuration to the hook thread ("
                                 << static_cast<unsigned long>(GetLastError()) << ")";
    }
}

unsigned __stdcall GestureEngine::thread_proc(void* argument) {
    static_cast<GestureEngine*>(argument)->run_worker();
    return 0;
}

void GestureEngine::run_worker() {
    worker_thread_id_ = GetCurrentThreadId();
    worker_ready_ = initialize_worker();
    SetEvent(ready_event_);
    if (!worker_ready_) {
        cleanup_worker();
        return;
    }

    MSG message = {};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result < 0) {
        post_log("Mouse Gesture Host: hook message loop failed (" +
                 std::to_string(static_cast<unsigned long>(GetLastError())) + ")");
    }
    cleanup_worker();
}

bool GestureEngine::initialize_worker() {
    worker_sink_ = CreateWindowExW(0, kSinkClass, L"", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, nullptr, instance_, nullptr);
    if (worker_sink_ == nullptr) {
        worker_error_ = GetLastError();
        return false;
    }

    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &GestureEngine::mouse_hook_proc, instance_, 0);
    if (hook_ == nullptr) {
        worker_error_ = GetLastError();
        return false;
    }

    recognizer_.set_stroke_length(worker_snapshot_->settings().stroke_length);
    if (SetTimer(worker_sink_, kHealthTimer, kHealthTimerMs, nullptr) == 0) {
        worker_error_ = GetLastError();
        return false;
    }

    GetCursorPos(&last_cursor_pos_);
    last_left_down_ = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    last_right_down_ = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    health_.note_event(GetTickCount64());
    return true;
}

void GestureEngine::cleanup_worker() {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (worker_sink_ != nullptr) {
        KillTimer(worker_sink_, kHealthTimer);
        DestroyWindow(worker_sink_);
        worker_sink_ = nullptr;
    }
    mouse_queue_.clear();
    mouse_process_posted_ = false;
    session_.reset();
    recognizer_.cancel();
    reported_gestures_.clear();
}

void GestureEngine::request_right_click_replay(SessionPoint point, const ReplayRequest* timing) {
    ReplayRequest request;
    if (timing != nullptr) request = *timing;
    request.point = point;
    request.queued_ms = GetTickCount64();

    bool post = false;
    {
        std::lock_guard<std::mutex> lock(main_queue_mutex_);
        if (pending_replays_.size() >= kMaxPendingReplays) {
            ++dropped_replays_;
        } else {
            pending_replays_.push_back(request);
        }
        if (!main_queue_posted_) {
            main_queue_posted_ = true;
            post = true;
        }
    }
    if (post && main_sink_ != nullptr && !PostMessageW(main_sink_, kMsgExecuteGesture, 0, 0)) {
        std::lock_guard<std::mutex> lock(main_queue_mutex_);
        main_queue_posted_ = false;
    }
}

void GestureEngine::queue_gesture(const std::string& gesture) {
    bool post = false;
    {
        std::lock_guard<std::mutex> lock(main_queue_mutex_);
        if (pending_commands_.size() >= kMaxPendingCommands) {
            ++dropped_commands_;
        } else {
            pending_commands_.push_back({ gesture, worker_snapshot_ });
        }
        if (!main_queue_posted_) {
            main_queue_posted_ = true;
            post = true;
        }
    }
    if (post && main_sink_ != nullptr && !PostMessageW(main_sink_, kMsgExecuteGesture, 0, 0)) {
        std::lock_guard<std::mutex> lock(main_queue_mutex_);
        main_queue_posted_ = false;
    }
}

void GestureEngine::post_log(const std::string& message) {
    bool post = false;
    {
        std::lock_guard<std::mutex> lock(main_queue_mutex_);
        if (pending_logs_.size() >= kMaxPendingLogs) {
            ++dropped_logs_;
        } else {
            pending_logs_.push_back(message);
        }
        if (!main_queue_posted_) {
            main_queue_posted_ = true;
            post = true;
        }
    }
    if (post && main_sink_ != nullptr && !PostMessageW(main_sink_, kMsgExecuteGesture, 0, 0)) {
        std::lock_guard<std::mutex> lock(main_queue_mutex_);
        main_queue_posted_ = false;
    }
}

void GestureEngine::drain_main_queue() {
    for (;;) {
        ReplayRequest replay;
        GestureCommandRequest command;
        std::string log;
        bool have_replay = false;
        bool have_command = false;
        bool have_log = false;
        std::uint64_t dropped_replays = 0;
        std::uint64_t dropped_commands = 0;
        std::uint64_t dropped_logs = 0;
        {
            std::lock_guard<std::mutex> lock(main_queue_mutex_);
            if (!pending_replays_.empty()) {
                replay = std::move(pending_replays_.front());
                pending_replays_.pop_front();
                have_replay = true;
            } else if (!pending_commands_.empty()) {
                command = std::move(pending_commands_.front());
                pending_commands_.pop_front();
                have_command = true;
            } else if (!pending_logs_.empty()) {
                log = std::move(pending_logs_.front());
                pending_logs_.pop_front();
                have_log = true;
            } else {
                dropped_replays = dropped_replays_;
                dropped_commands = dropped_commands_;
                dropped_logs = dropped_logs_;
                dropped_replays_ = 0;
                dropped_commands_ = 0;
                dropped_logs_ = 0;
                main_queue_posted_ = false;
            }
        }

        if (have_replay) {
            replay_right_click(replay);
            continue;
        }
        if (have_log) {
            FB2K_console_formatter() << log.c_str();
            continue;
        }
        if (have_command) {
            const BindingSnapshotPtr snapshot = command.snapshot;
            const auto matches = snapshot->exact_matches(command.gesture);
            for (std::size_t i = 0; i < matches.size(); ++i) {
                if (i > 0 && snapshot->settings().first_only) break;
                const GestureBinding& binding = snapshot->settings().bindings[matches[i]];
                std::string diagnostic;
                if (!execute_binding(binding, &diagnostic)) {
                    FB2K_console_formatter()
                        << "Mouse Gesture Host: gesture \"" << command.gesture.c_str()
                        << "\" matched, but command \"" << binding.command_path.c_str()
                        << "\" was not executed: " << diagnostic.c_str();
                }
            }
            continue;
        }

        if (dropped_replays != 0) {
            FB2K_console_formatter() << "Mouse Gesture Host: dropped "
                                     << static_cast<t_size>(dropped_replays)
                                     << " queued right-click replay(s) because the main-thread queue was full";
        }
        if (dropped_commands != 0) {
            FB2K_console_formatter() << "Mouse Gesture Host: dropped "
                                     << static_cast<t_size>(dropped_commands)
                                     << " queued gesture(s) because the main-thread queue was full";
        }
        if (dropped_logs != 0) {
            FB2K_console_formatter() << "Mouse Gesture Host: dropped "
                                     << static_cast<t_size>(dropped_logs) << " queued log message(s)";
        }
        break;
    }
}

void GestureEngine::process_mouse_queue() {
    // Разрешаем следующему событию поставить wake-up уже во время drain.
    mouse_process_posted_ = false;

    QueuedMouseEvent event;
    while (mouse_queue_.pop(event)) {
        for (std::size_t i = 0; i < event.result.action_count; ++i) {
            const SessionAction& action = event.result.actions[i];
            if (action.kind == SessionActionKind::finish && event.right_up_ms != 0) {
                ReplayRequest timing;
                timing.right_up_ms = event.right_up_ms;
                timing.finish_started_ms = GetTickCount64();
                timing.events_ahead = event.events_ahead;
                process_action(action, &timing);
            } else {
                process_action(action);
            }
        }
    }
    if (reported_mouse_drops_ != mouse_queue_.dropped()) {
        const std::uint64_t delta = mouse_queue_.dropped() - reported_mouse_drops_;
        reported_mouse_drops_ = mouse_queue_.dropped();
        post_log("Mouse Gesture Host: dropped " + std::to_string(delta) +
                 " low-level mouse event(s); total " + std::to_string(reported_mouse_drops_));
    }
}

void GestureEngine::process_action(const SessionAction& action, const ReplayRequest* timing) {
    switch (action.kind) {
    case SessionActionKind::begin:
        recognizer_.set_stroke_length(worker_snapshot_->settings().stroke_length);
        recognizer_.begin(action.point.x, action.point.y);
        return;
    case SessionActionKind::move:
        recognizer_.move(action.point.x, action.point.y);
        return;
    case SessionActionKind::emit_rocker:
        queue_gesture(token_id(action.rocker_token));
        return;
    case SessionActionKind::emit_wheel:
        queue_gesture(wheel_sequence_gesture(action.wheel_sequence));
        return;
    case SessionActionKind::unmatched_wheel:
        report_unmatched_gesture(wheel_sequence_gesture(action.wheel_sequence));
        return;
    case SessionActionKind::finish:
        break;
    }

    recognizer_.finish();
    const std::string gesture = recognizer_.canonical();
    const auto match_candidates = recognizer_.match_candidates();
    recognizer_.cancel();

    bool executed = false;
    for (const auto& candidate : match_candidates) {
        if (!candidate.empty() && worker_snapshot_->has_binding(candidate)) {
            queue_gesture(candidate);
            executed = true;
            break;
        }
    }
    if (!executed && !gesture.empty()) report_unmatched_gesture(gesture);

    if (action.was_suppressed && !executed && !action.wheel_used && action.was_tap) {
        request_right_click_replay(action.right_down_point, timing);
    }
}

void GestureEngine::report_unmatched_gesture(const std::string& gesture) {
    if (gesture.empty()) return;
    for (const auto& seen : reported_gestures_) {
        if (seen == gesture) return;
    }
    if (reported_gestures_.size() >= 32) reported_gestures_.erase(reported_gestures_.begin());
    reported_gestures_.push_back(gesture);
    post_log("Mouse Gesture Host: gesture \"" + gesture + "\" has no binding");
}

void GestureEngine::replay_right_click(const ReplayRequest& request) {
    // SendInput выполняется на main thread: worker остаётся свободным для synthetic hook events.
    const bool timed = request.right_up_ms != 0;
    const std::uint64_t replay_started_ms = timed ? GetTickCount64() : 0;
    const SessionPoint point = request.point;
    const POINT native_point{ point.x, point.y };
    if (!over_main_window(main_window_, native_point)) {
        post_log("Mouse Gesture Host: right-click replay cancelled because the saved point left the main window");
        return;
    }
    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) {
        post_log("Mouse Gesture Host: right-click replay cancelled because the physical button is still down");
        return;
    }

    const int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtual_width <= 0 || virtual_height <= 0) {
        post_log("Mouse Gesture Host: right-click replay cancelled because virtual-screen metrics are invalid");
        return;
    }

    const auto normalize = [](long value, int origin, int extent) {
        const long long relative = static_cast<long long>(value) - origin;
        return static_cast<LONG>((relative * 65535 + extent / 2) / extent);
    };

    INPUT inputs[2] = {};
    for (auto& input : inputs) {
        input.type = INPUT_MOUSE;
        input.mi.dx = normalize(point.x, virtual_left, virtual_width);
        input.mi.dy = normalize(point.y, virtual_top, virtual_height);
        input.mi.dwExtraInfo = kOwnMouseMarker;
    }
    inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_MOVE |
                           MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MOVE |
                           MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    const std::uint64_t send_started_ms = timed ? GetTickCount64() : 0;
    const UINT sent = SendInput(2, inputs, sizeof(INPUT));
    if (timed) {
        const std::uint64_t completed_ms = GetTickCount64();
        const std::uint64_t total_ms = completed_ms - request.right_up_ms;
        if (total_ms >= kSlowReplayLogMs) {
            post_log("Mouse Gesture Host: slow right-click replay: total " +
                     std::to_string(total_ms) + " ms (queue/moves " +
                     std::to_string(request.finish_started_ms - request.right_up_ms) +
                     ", finish " + std::to_string(request.queued_ms - request.finish_started_ms) +
                     ", main queue " + std::to_string(replay_started_ms - request.queued_ms) +
                     ", prepare " + std::to_string(send_started_ms - replay_started_ms) +
                     ", SendInput " + std::to_string(completed_ms - send_started_ms) +
                     "), " + std::to_string(request.events_ahead) +
                     " event(s) ahead at right-up");
        }
    }
    if (sent != 2) {
        post_log("Mouse Gesture Host: failed to replay the right click (" +
                 std::to_string(static_cast<unsigned long>(GetLastError())) + ")");
    }
}

void GestureEngine::health_check() {
    if (mouse_queue_.size() != 0) process_mouse_queue();

    const bool left_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool right_down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    SessionResult reconciled = session_.reconcile_left_button(left_down, *worker_snapshot_);
    for (std::size_t i = 0; i < reconciled.action_count; ++i) {
        process_action(reconciled.actions[i]);
    }

    POINT cursor = last_cursor_pos_;
    GetCursorPos(&cursor);
    const bool activity = cursor.x != last_cursor_pos_.x || cursor.y != last_cursor_pos_.y ||
                          left_down != last_left_down_ || right_down != last_right_down_;
    last_cursor_pos_ = cursor;
    last_left_down_ = left_down;
    last_right_down_ = right_down;

    const std::uint64_t now = GetTickCount64();
    if (!health_.should_reinstall(now, activity)) return;

    post_log("Mouse Gesture Host: input activity arrived without hook events; reinstalling the hook");
    const bool success = reinstall_hook();
    health_.note_reinstall(now, success);
    if (!success) {
        post_log("Mouse Gesture Host: next hook reinstall attempt in " +
                 std::to_string(health_.backoff_ms()) + " ms");
    }
}

bool GestureEngine::reinstall_hook() {
    if (hook_ != nullptr) {
        if (!UnhookWindowsHookEx(hook_)) {
            post_log("Mouse Gesture Host: failed to unhook before reinstall (" +
                     std::to_string(static_cast<unsigned long>(GetLastError())) + ")");
        }
        hook_ = nullptr;
    }

    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 &&
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0) {
        mouse_queue_.clear();
        mouse_process_posted_ = false;
        session_.reset();
        recognizer_.cancel();
    }

    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &GestureEngine::mouse_hook_proc, instance_, 0);
    if (hook_ == nullptr) {
        post_log("Mouse Gesture Host: failed to reinstall the mouse hook (" +
                 std::to_string(static_cast<unsigned long>(GetLastError())) + ")");
        return false;
    }
    post_log("Mouse Gesture Host: mouse hook reinstalled");
    return true;
}

LRESULT CALLBACK GestureEngine::mouse_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
        GestureEngine& engine = instance();
        if (info != nullptr && info->dwExtraInfo != kOwnMouseMarker) {
            const std::uint64_t event_ms = GetTickCount64();
            engine.health_.note_event(event_ms);
            // Пустые move не накапливаются перед RUp и не будят worker без необходимости.
            if (!engine.worker_snapshot_->settings().disabled &&
                (wparam != WM_MOUSEMOVE || engine.session_.tracks_mouse_move())) {
                SessionEvent event;
                if (make_session_event(wparam, *info, engine.main_window_, event)) {
                    const GestureSession previous = engine.session_;
                    SessionResult result = engine.session_.handle_event(event, *engine.worker_snapshot_);
                    QueuedMouseEvent queued;
                    queued.result = result;
                    queued.droppable = (wparam == WM_MOUSEMOVE);
                    if (wparam == WM_RBUTTONUP) {
                        queued.right_up_ms = event_ms;
                        queued.events_ahead = engine.mouse_queue_.size();
                    }
                    if (engine.mouse_queue_.push(queued)) {
                        // У критического события всегда есть собственный wake-up:
                        // RUp не должен ждать health timer из-за старого posted-флага.
                        const bool needs_wakeup = !queued.droppable || !engine.mouse_process_posted_;
                        if (needs_wakeup) {
                            if (PostMessageW(engine.worker_sink_, kMsgProcessMouse, 0, 0)) {
                                engine.mouse_process_posted_ = true;
                            } else {
                                engine.mouse_process_posted_ = false;
                            }
                        }
                        if (result.swallow) return 1;
                    } else if (!queued.droppable) {
                        // Fail open для критического события; состояние не должно
                        // указывать на действие, которое не попало в очередь. Потерянный
                        // move всё равно отмечает drag, чтобы tap replay не был ложным.
                        engine.session_ = previous;
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK GestureEngine::sink_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    GestureEngine& engine = instance();
    if (hwnd == engine.main_sink_ && msg == kMsgExecuteGesture) {
        engine.drain_main_queue();
        return 0;
    }
    if (hwnd == engine.worker_sink_) {
        switch (msg) {
        case kMsgProcessMouse:
            engine.process_mouse_queue();
            return 0;
        case kMsgReload: {
            std::unique_ptr<ReloadMessage> message(reinterpret_cast<ReloadMessage*>(lparam));
            if (message) {
                engine.worker_snapshot_ = message->snapshot;
                engine.recognizer_.set_stroke_length(engine.worker_snapshot_->settings().stroke_length);
                if (engine.worker_snapshot_->settings().disabled) {
                    engine.mouse_queue_.clear();
                    engine.session_.reset();
                    engine.recognizer_.cancel();
                }
            }
            return 0;
        }
        case kMsgShutdown:
            PostQuitMessage(0);
            return 0;
        case WM_TIMER:
            if (wparam == kHealthTimer) engine.health_check();
            return 0;
        default:
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace mgh
