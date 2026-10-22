#include "command_catalog.h"

namespace mgh {
namespace {

//! База и запас идентификаторов для сгенерированных Win32-меню.
constexpr UINT kMenuBase = 4096;
constexpr UINT kMenuSpan = 8192;

struct MainMenuRoot {
    const GUID* guid;
    const char* label;
};

//! Шесть стандартных корневых групп главного меню foobar2000.
const MainMenuRoot kMainMenuRoots[] = {
    { &mainmenu_groups::file,     "File" },
    { &mainmenu_groups::edit,     "Edit" },
    { &mainmenu_groups::view,     "View" },
    { &mainmenu_groups::playback, "Playback" },
    { &mainmenu_groups::library,  "Library" },
    { &mainmenu_groups::help,     "Help" },
};

bool read_menu_item(HMENU menu, int index, MENUITEMINFOW& info, std::string& text) {
    wchar_t buffer[512] = {};
    info = {};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
    info.dwTypeData = buffer;
    info.cch = static_cast<UINT>(sizeof(buffer) / sizeof(buffer[0]) - 1);
    if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info)) return false;
    text = strip_mnemonics(utf8_from_wide(buffer));
    return true;
}

bool generate_root(mainmenu_manager::ptr& manager, HMENU menu, const GUID& root) {
    manager = mainmenu_manager::get();
    if (!manager.is_valid()) return false;
    manager->instantiate(root);
    manager->generate_menu_win32(menu, kMenuBase, kMenuSpan, mainmenu_manager::flag_view_full);
    return true;
}

void walk_main_menu(HMENU menu, const std::string& prefix, std::vector<CatalogEntry>& out) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info{};
        std::string name;
        if (!read_menu_item(menu, i, info, name)) continue;
        if ((info.fType & MFT_SEPARATOR) != 0) continue;
        if (name.empty()) continue;

        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (info.hSubMenu != nullptr) {
            walk_main_menu(info.hSubMenu, path, out);
        } else {
            CatalogEntry entry;
            entry.path = path;
            entry.group = "main";
            out.push_back(std::move(entry));
        }
    }
}

bool init_context(contextmenu_manager& manager, const std::string& group_id) {
    if (group_id == "context_playlist") {
        manager.init_context_playlist(contextmenu_manager::flag_view_full);
        return true;
    }
    if (group_id == "context_now_playing") {
        return manager.init_context_now_playing(contextmenu_manager::flag_view_full);
    }

    ui_selection_manager::ptr selection = ui_selection_manager::get();
    if (!selection.is_valid()) return false;

    metadb_handle_list items;
    selection->get_selection(items);
    if (items.get_count() == 0) return false;

    manager.init_context(items, contextmenu_manager::flag_view_full);
    return true;
}

void walk_context(contextmenu_node& node, const std::string& prefix, const std::string& group_id,
                  std::vector<CatalogEntry>& out) {
    const t_size count = node.get_num_children();
    for (t_size i = 0; i < count; ++i) {
        contextmenu_node* child = node.get_child(i);
        if (child == nullptr) continue;

        const contextmenu_item_node::t_type type = child->get_type();
        if (type == contextmenu_item_node::type_separator) continue;

        const char* raw = child->get_name();
        const std::string name = raw != nullptr ? strip_mnemonics(raw) : std::string();
        if (name.empty()) continue;

        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (type == contextmenu_item_node::type_group) {
            walk_context(*child, path, group_id, out);
        } else {
            CatalogEntry entry;
            entry.path = path;
            entry.group = group_id;
            out.push_back(std::move(entry));
        }
    }
}

std::vector<CatalogEntry> list_main_menu() {
    std::vector<CatalogEntry> out;
    for (const auto& root : kMainMenuRoots) {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) continue;

        try {
            mainmenu_manager::ptr manager;
            if (generate_root(manager, menu, *root.guid)) {
                walk_main_menu(menu, root.label, out);
            }
        } catch (...) {
            // Один "проблемный" поставщик команд не должен ломать весь список.
        }
        DestroyMenu(menu);
    }
    return out;
}

std::vector<CatalogEntry> list_context_menu(const std::string& group_id) {
    std::vector<CatalogEntry> out;

    contextmenu_manager::ptr manager;
    contextmenu_manager::g_create(manager);
    if (!manager.is_valid()) return out;

    if (!init_context(*manager, group_id)) return out;

    contextmenu_node* root = manager->get_root();
    if (root == nullptr) return out;

    walk_context(*root, std::string(), group_id, out);
    return out;
}

bool find_main_item(HMENU menu, const std::string& prefix, const std::string& target_path,
                    const std::string& target_leaf, bool by_leaf, UINT& id) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info{};
        std::string name;
        if (!read_menu_item(menu, i, info, name)) continue;
        if ((info.fType & MFT_SEPARATOR) != 0) continue;
        if (name.empty()) continue;

        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (info.hSubMenu != nullptr) {
            if (find_main_item(info.hSubMenu, path, target_path, target_leaf, by_leaf, id)) return true;
        } else {
            const bool matches = by_leaf ? (name == target_leaf) : (path == target_path);
            if (matches) {
                id = info.wID;
                return true;
            }
        }
    }
    return false;
}

contextmenu_node* find_context_item(contextmenu_node& node, const std::string& prefix,
                                    const std::string& target_path, const std::string& target_leaf,
                                    bool by_leaf) {
    const t_size count = node.get_num_children();
    for (t_size i = 0; i < count; ++i) {
        contextmenu_node* child = node.get_child(i);
        if (child == nullptr) continue;

        const contextmenu_item_node::t_type type = child->get_type();
        if (type == contextmenu_item_node::type_separator) continue;

        const char* raw = child->get_name();
        const std::string name = raw != nullptr ? strip_mnemonics(raw) : std::string();
        if (name.empty()) continue;

        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (type == contextmenu_item_node::type_group) {
            if (contextmenu_node* found = find_context_item(*child, path, target_path, target_leaf, by_leaf)) {
                return found;
            }
        } else {
            const bool matches = by_leaf ? (name == target_leaf) : (path == target_path);
            if (matches) return child;
        }
    }
    return nullptr;
}

bool execute_main_menu(const std::string& path) {
    const std::string leaf = path_leaf(path);

    for (const auto& root : kMainMenuRoots) {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) continue;

        bool executed = false;
        try {
            mainmenu_manager::ptr manager;
            if (generate_root(manager, menu, *root.guid)) {
                const std::string prefix = std::string(root.label) + "/";
                UINT id = 0;
                if (find_main_item(menu, prefix, path, leaf, false, id) ||
                    find_main_item(menu, prefix, path, leaf, true, id)) {
                    executed = manager->execute_command(id - kMenuBase);
                }
            }
        } catch (...) {
            executed = false;
        }
        DestroyMenu(menu);

        if (executed) return true;
    }
    return false;
}

bool execute_context_menu(const std::string& group_id, const std::string& place_id, const std::string& path) {
    std::string context = group_id;
    if (place_id == "playlist") {
        context = "context_playlist";
    } else if (place_id == "now_playing") {
        context = "context_now_playing";
    }

    contextmenu_manager::ptr manager;
    contextmenu_manager::g_create(manager);
    if (!manager.is_valid()) return false;
    if (!init_context(*manager, context)) return false;

    contextmenu_node* root = manager->get_root();
    if (root == nullptr) return false;

    const std::string leaf = path_leaf(path);
    contextmenu_node* item = find_context_item(*root, std::string(), path, leaf, false);
    if (item == nullptr) item = find_context_item(*root, std::string(), path, leaf, true);
    if (item == nullptr) return false;

    try {
        item->execute();
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace

std::string path_parent(std::string_view path) {
    const std::size_t pos = path.rfind('/');
    if (pos == std::string_view::npos) return std::string();
    return std::string(path.substr(0, pos));
}

std::string path_leaf(std::string_view path) {
    const std::size_t pos = path.rfind('/');
    if (pos == std::string_view::npos) return std::string(path);
    return std::string(path.substr(pos + 1));
}

std::vector<CatalogEntry> list_commands(std::string_view group_id) {
    try {
        if (group_id == "main") return list_main_menu();
        return list_context_menu(std::string(group_id));
    } catch (...) {
        return {};
    }
}

bool execute_binding(const GestureBinding& binding) {
    if (binding.command_path.empty()) return false;

    if (binding.group == "main") {
        return execute_main_menu(binding.command_path);
    }
    return execute_context_menu(binding.group, binding.place, binding.command_path);
}

} // namespace mgh
