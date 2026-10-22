#include "common.h"

#include "gesture_engine.h"

// ---------------------------------------------------------------------------
//  Метаданные компонента
// ---------------------------------------------------------------------------

DECLARE_COMPONENT_VERSION(
    "Mouse Gesture Host",
    "1.0.0",
    "Mouse gestures for foobar2000 (v2.0+ / x64).\n"
    "\n"
    "Right-button drag gestures, diagonal strokes, wheel and rocker gestures.\n"
    "Gestures are bound to any main menu or context menu command.\n"
    "\n"
    "Preferences: Tools -> Mouse Gesture Host."
);

// Имя файла жёстко фиксировано: так компонент корректно опознаётся
// в troubleshooter'е и не может быть случайно переименован.
VALIDATE_COMPONENT_FILENAME("foo_mouse_gesture_host.dll");

// ---------------------------------------------------------------------------
//  Инициализация / завершение
// ---------------------------------------------------------------------------

namespace {

class MouseGestureInitQuit : public initquit {
public:
    void on_init() override {
        mgh::GestureEngine::instance().install();
    }

    void on_quit() override {
        mgh::GestureEngine::instance().uninstall();
    }
};

initquit_factory_t<MouseGestureInitQuit> g_initquit_factory;

} // namespace
