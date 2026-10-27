#pragma once

#include "common.h"
#include "binding_index.h"
#include "gesture_config.h"
#include "gesture_recognizer.h"
#include "gesture_session.h"

#include <mutex>

namespace mgh {

struct GestureCommandRequest {
    std::string gesture;
    BindingSnapshotPtr snapshot;
};

//! Win32 adapter: WH_MOUSE_LL живёт на выделенном message-pump thread,
//! а SDK menu services и команды исполняются только в главном потоке.
class GestureEngine {
public:
    static GestureEngine& instance();

    void install();
    void uninstall();
    void reload();

    bool installed() const { return thread_ != nullptr; }

    static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK sink_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    GestureEngine() = default;
    ~GestureEngine() = default;
    GestureEngine(const GestureEngine&) = delete;
    GestureEngine& operator=(const GestureEngine&) = delete;

    static unsigned __stdcall thread_proc(void* argument);
    void run_worker();
    bool initialize_worker();
    void cleanup_worker();

    struct ReplayRequest {
        SessionPoint point;
        std::uint64_t right_up_ms = 0;
        std::uint64_t finish_started_ms = 0;
        std::size_t events_ahead = 0;
        std::uint64_t queued_ms = 0;
    };

    void process_mouse_queue();
    void process_action(const SessionAction& action, const ReplayRequest* timing = nullptr);
    void request_right_click_replay(SessionPoint point, const ReplayRequest* timing);
    void queue_gesture(const std::string& gesture);
    void post_log(const std::string& message);
    void drain_main_queue();
    void report_unmatched_gesture(const std::string& gesture);

    void replay_right_click(const ReplayRequest& request);
    void health_check();
    bool reinstall_hook();

    // Только главный поток: конфигурация, SDK окна и исполнение команд.
    HINSTANCE instance_ = nullptr;
    HWND main_window_ = nullptr;
    HWND main_sink_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE ready_event_ = nullptr;
    DWORD worker_thread_id_ = 0;
    DWORD worker_error_ = 0;
    bool worker_ready_ = false;
    BindingSnapshotPtr snapshot_ = std::make_shared<BindingSnapshot>(GestureConfig::defaults());

    // Ограниченный cross-thread канал. SDK console вызывается после pop на main thread.
    std::mutex main_queue_mutex_;
    std::deque<ReplayRequest> pending_replays_;
    std::deque<GestureCommandRequest> pending_commands_;
    std::deque<std::string> pending_logs_;
    bool main_queue_posted_ = false;
    std::uint64_t dropped_replays_ = 0;
    std::uint64_t dropped_commands_ = 0;
    std::uint64_t dropped_logs_ = 0;

    // Только worker thread. Hook callback и message loop не выполняются одновременно,
    // поэтому fixed ring и session не требуют mutex.
    HHOOK hook_ = nullptr;
    HWND worker_sink_ = nullptr;
    BindingSnapshotPtr worker_snapshot_ = std::make_shared<BindingSnapshot>(GestureConfig::defaults());
    GestureSession session_;
    GestureRecognizer recognizer_;
    BoundedMouseQueue<256> mouse_queue_;
    bool mouse_process_posted_ = false;
    std::uint64_t reported_mouse_drops_ = 0;
    std::vector<std::string> reported_gestures_;
    HookHealth health_;
    POINT last_cursor_pos_ = {};
    bool last_left_down_ = false;
    bool last_right_down_ = false;
};

} // namespace mgh
