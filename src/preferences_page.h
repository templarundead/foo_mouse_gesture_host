#pragma once

#include "common.h"

namespace mgh {

//! Страница настроек компонента (Preferences -> Tools -> Mouse Gesture Host).
class GesturePreferencesPage : public preferences_page_v3 {
public:
    const char* get_name() override;
    GUID get_guid() override;
    GUID get_parent_guid() override;

    preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent,
                                               preferences_page_callback::ptr callback) override;
};

} // namespace mgh
