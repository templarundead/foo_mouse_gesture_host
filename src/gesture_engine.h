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
    //! Отправить обычный правый клик (после подавленного правого нажатия).
    void replay_right_click();

    //! Поставить отложенный реплей клика в очередь главного потока.
    //! SendInput нельзя вызывать изнутри WH_MOUSE_LL: система ждёт возврата
    //! из колбэка (LowLevelHooksTimeout), и контекстное меню открывается
    //! с заметной задержкой.
    void request_right_click_replay();

    bool has_binding(const std::string& gesture) const;
    bool has_binding_prefix(const std::string& gesture) const;

    //! Обработать один "щелчок" колеса (WHEEL_DELTA). Возвращает true, если
    //! событие нужно съесть (иначе хост прокрутит список как обычно).
    bool handle_wheel_notch(bool up);

    //! Сообщить в консоль о жесте, для которого нет привязки (один раз на жест).
    void report_unmatched_gesture(const std::string& gesture);
    void reset_session();

    HHOOK hook_ = nullptr;
    HWND sink_ = nullptr;

    GestureSettings settings_;
    GestureRecognizer recognizer_;

    //! ПКМ зажата над окном foobar2000, и её события подавлены, чтобы хост
    //! не начал штатное перетаскивание треков. Если жест не распознан,
    //! на отпускании синтезируется обычный правый клик (контекстное меню).
    bool suppress_right_ = false;
    POINT right_down_point_ = {};
    bool right_moved_ = false;

    //! Момент последнего SendInput-клика. Инжектированные события игнорируем
    //! только в этом окне: часть драйверов (тачпады, мыши) тоже помечает
    //! свой ввод как LLMHF_INJECTED, и глобальный фильтр ломал бы жесты.
    ULONGLONG replay_guard_ = 0;

    //! Правый клик нужно переиграть, как только колбэк хука вернёт управление.
    bool replay_pending_ = false;

    bool right_down_ = false;
    bool left_down_ = false;
    bool session_from_right_ = true;
    bool rocker_armed_ = false;
    bool wheel_used_ = false;
    std::string wheel_sequence_;

    //! Накопленная дельта колеса. Мыши и тачпады с высоким разрешением шлют
    //! дробные дельты (1..30 вместо 120), поэтому звено появляется только
    //! когда набрался целый щелчок - иначе один щелчок превращался в десяток
    //! звеньев "W↑" и ни одна привязка не совпадала.
    int wheel_accum_ = 0;

    //! Жесты, о которых уже сообщили в консоль (чтобы не спамить).
    std::vector<std::string> reported_gestures_;

    std::deque<std::string> pending_;
};

} // namespace mgh
