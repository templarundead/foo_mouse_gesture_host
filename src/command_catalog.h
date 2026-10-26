#pragma once

#include "command_identity.h"

namespace mgh {

//! Динамически собрать список команд указанной группы через SDK 2.0 menu_tree_item.
//! Для контекстных групп используется текущее выделение / активный плейлист / трек.
std::vector<CatalogEntry> list_commands(std::string_view group_id);

//! Выполнить команду, привязанную к жесту. Только главный поток foobar2000.
//! При false diagnostic содержит причину unavailable/conflict/ошибки исполнения.
bool execute_binding(const GestureBinding& binding, std::string* diagnostic = nullptr);

//! Путь без последнего сегмента ("A/B/C" -> "A/B").
std::string path_parent(std::string_view path);

//! Последний сегмент пути ("A/B/C" -> "C").
std::string path_leaf(std::string_view path);

} // namespace mgh
