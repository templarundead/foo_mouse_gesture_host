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
    //! SDK command GUID в каноническом текстовом виде; пусто, если SDK его не предоставляет.
    std::string command_guid;
    //! SDK subcommand GUID для динамической команды; без command_guid не используется.
    std::string subcommand_guid;
    //! Путь индексов в дереве меню (например "0.4.2") для структурной проверки.
    std::string node_path;

    GestureBinding() = default;
    GestureBinding(std::string p_gesture, std::string p_group, std::string p_command_path,
                   std::string p_place, std::string p_command_guid = {},
                   std::string p_subcommand_guid = {}, std::string p_node_path = {})
        : gesture(std::move(p_gesture)), group(std::move(p_group)),
          command_path(std::move(p_command_path)), place(std::move(p_place)),
          command_guid(std::move(p_command_guid)), subcommand_guid(std::move(p_subcommand_guid)),
          node_path(std::move(p_node_path)) {}

    bool operator==(const GestureBinding&) const = default;
};

//! Полный набор настроек компонента.
struct GestureSettings {
    int stroke_length = 8;       //!< "Stroke Length" (1..100).
    bool disabled = false;       //!< "Disable mouse gesture".
    bool alphabet = false;       //!< "Represent direction by the alphabet".
    bool first_only = false;     //!< "Execute only the first command".
    std::vector<GestureBinding> bindings;
};

enum class BindingSchema {
    empty,
    legacy_v1,
    schema_v2,
    unknown,
};

struct BindingDiagnostic {
    std::size_t line = 0;
    std::string message;
};

struct BindingParseResult {
    BindingSchema schema = BindingSchema::empty;
    std::vector<GestureBinding> bindings;
    std::vector<BindingDiagnostic> diagnostics;
    std::size_t dropped_rows = 0;
    std::size_t duplicate_rows = 0;
    //! false при malformed/corrupt строках. Полные дубликаты не делают таблицу corrupt.
    bool valid = true;
};

//! Хранилище настроек поверх cfg_var foobar2000 + кэш для движка.
//! Изменения выполняет главный поток. Чистая сериализация доступна и без SDK.
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

    //! Schema v2 и строгий разбор legacy/v2; реализация не зависит от SDK.
    static std::string serialize(const std::vector<GestureBinding>& bindings);
    static BindingParseResult deserialize(std::string_view text);

private:
    GestureConfig();

    GestureSettings cached_;
};

//! Группы команд (для выпадающего списка "Group"). Пары "идентификатор, подпись".
const std::vector<std::pair<const char*, const char*>>& command_groups();

//! Места исполнения (для выпадающего списка "Place"). Пары "идентификатор, подпись".
const std::vector<std::pair<const char*, const char*>>& command_places();

bool is_valid_command_group(std::string_view group_id);
bool is_valid_command_place(std::string_view place_id);

const char* group_label(std::string_view group_id);
std::string group_id_from_label(std::string_view label);

const char* place_label(std::string_view place_id);
std::string place_id_from_label(std::string_view label);

} // namespace mgh
