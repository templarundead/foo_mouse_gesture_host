#pragma once

#include "gesture_tokens.h"

namespace mgh {

//! Одна привязка "жест -> команда".
struct GestureBinding {
    //! Каноническая запись жеста, например "U D", "WU", "R+L".
    std::string gesture;
    //! Идентификатор группы команд: main | context_ui | context_playlist | context_now_playing.
    std::string group;
    //! Полный путь команды внутри группы ("Playback/Next", "Utilities/Optimize file layout").
    std::string command_path;
    //! Идентификатор места исполнения: main_window | playlist | now_playing.
    std::string place;
};

//! Полный набор настроек компонента.
struct GestureSettings {
    int stroke_length = 10;      //!< "Stroke Length" (1..100).
    bool disabled = false;       //!< "Disable mouse gesture".
    bool alphabet = false;       //!< "Represent direction by the alphabet".
    bool first_only = false;     //!< "Execute only the first command".
    std::vector<GestureBinding> bindings;
};

//! Хранилище настроек поверх cfg_var foobar2000 + кэш для движка.
//!
//! Все методы, меняющие состояние, вызываются из главного потока
//! (страница настроек). Движок читает cached(); мышиный хук приходит
//! в тот же главный поток, поэтому дополнительная синхронизация не нужна.
class GestureConfig {
public:
    static GestureConfig& instance();

    //! Прочитать настройки из конфигурации foobar2000.
    GestureSettings load_from_storage() const;

    //! Сохранить настройки и обновить кэш.
    void apply(const GestureSettings& settings);

    //! Последние применённые настройки.
    const GestureSettings& cached() const { return cached_; }

    //! Настройки по умолчанию.
    static GestureSettings defaults();

    //! Сериализация таблицы привязок в строку для cfg_string.
    static std::string serialize(const std::vector<GestureBinding>& bindings);
    static std::vector<GestureBinding> deserialize(std::string_view text);

private:
    GestureConfig();

    GestureSettings cached_;
};

//! Группы команд (для выпадающего списка "Group"). Пары "идентификатор, подпись".
const std::vector<std::pair<const char*, const char*>>& command_groups();

//! Места исполнения (для выпадающего списка "Place").
const std::vector<std::pair<const char*, const char*>>& command_places();

const char* group_label(std::string_view group_id);
std::string group_id_from_label(std::string_view label);

const char* place_label(std::string_view place_id);
std::string place_id_from_label(std::string_view label);

} // namespace mgh
