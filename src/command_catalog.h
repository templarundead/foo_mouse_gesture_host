#pragma once

#include "gesture_config.h"

namespace mgh {

//! Одна команда, доступная для привязки к жесту.
struct CatalogEntry {
    //! Полный путь команды внутри меню, например "Playback/Next".
    std::string path;
    //! Идентификатор группы, к которой относится команда.
    std::string group;
};

//! Динамически собрать список команд указанной группы.
//!
//! Для группы "main" используется mainmenu_manager (главное меню
//! foobar2000), для контекстных групп - contextmenu_manager, инициализированный
//! текущим выделением / активным плейлистом / проигрываемым треком.
std::vector<CatalogEntry> list_commands(std::string_view group_id);

//! Выполнить команду, привязанную к жесту.
bool execute_binding(const GestureBinding& binding);

//! Путь без последнего сегмента ("A/B/C" -> "A/B").
std::string path_parent(std::string_view path);

//! Последний сегмент пути ("A/B/C" -> "C").
std::string path_leaf(std::string_view path);

} // namespace mgh
