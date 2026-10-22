#pragma once

#include "gesture_config.h"
#include "gesture_recognizer.h"

namespace mgh {

//! Движок жестов: глобальный низкоуровневый мышиный хук + исполнение команд.
//!
//! WH_MOUSE_LL выбран осознанно: он позволяет перехватывать движения мыши
//! поверх любого UI foobar2000 (Default UI, Columns UI, сторонние панели),
//! не требуя от host'а никаких расширений. Хук ставится и снимается из
//! главного потока, колбэк тоже приходит в главный поток, поэтому работа
//! с сервисами foobar2000 внутри него безопасна.
//!
//! Сами команды исполняются не внутри хука (это привело бы к таймаутам
//! LowLevelHooksTimeout и потенциальным deadlock'ам при открытии модальных
//! диалогов), а отложенно - через скрытое message-only окно.
class GestureEngine {
public:
    static GestureEngine& instance();

    //! Установить хук (вызывается из initquit::on_init).
    void install();

    //! Снять хук (вызывается из initquit::on_quit).
    void uninstall();

    //! Перечитать настройки после Apply / Reset на странице настроек.
    void reload();

    bool installed() const { return hook_ != nullptr; }

    //! Обработчики Win32 (публичные, т.к. на них ссылается WNDCLASSEX/SetWindowsHookEx).
    static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK sink_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    GestureEngine() = default;
    ~GestureEngine() = default;
    GestureEngine(const GestureEngine&) = delete;
    GestureEngine& operator=(const GestureEngine&) = delete;

    bool handle(WPARAM msg, const MSLLHOOKSTRUCT& info);
    void drain_queue();
    void queue_gesture(const std::string& gesture);

    bool has_binding(const std::string& gesture) const;
    bool has_binding_prefix(const std::string& gesture) const;
    void reset_session();

    HHOOK hook_ = nullptr;
    HWND sink_ = nullptr;

    GestureSettings settings_;
    GestureRecognizer recognizer_;

    bool right_down_ = false;
    bool left_down_ = false;
    bool session_from_right_ = true;
    bool rocker_armed_ = false;
    bool wheel_used_ = false;
    std::string wheel_sequence_;

    std::deque<std::string> pending_;
};

} // namespace mgh
