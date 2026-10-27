#include "common.h"
#include "command_catalog.h"

#include <exception>
#include <iterator>

namespace mgh {
namespace {

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

struct ResolvedEntry : CatalogEntry {
    menu_tree_item::ptr item;
};

struct ResolvedCatalog {
    //! Порядок членов важен: entries разрушаются раньше своих manager.
    std::vector<service_ptr> owners;
    std::vector<ResolvedEntry> entries;
    std::string error;
};

//! Алфавитный порядок для отображаемых путей команд.
bool command_path_less(const ResolvedEntry& left, const ResolvedEntry& right) {
    const std::wstring left_text = wide_from_utf8(left.path);
    const std::wstring right_text = wide_from_utf8(right.path);
    const int comparison = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                                         left_text.c_str(), -1, right_text.c_str(), -1);
    if (comparison == CSTR_LESS_THAN) return true;
    if (comparison == CSTR_GREATER_THAN) return false;
    return left.path < right.path;
}

void sort_command_catalog(std::vector<ResolvedEntry>& entries) {
    std::stable_sort(entries.begin(), entries.end(), command_path_less);
}

std::string format_node_path(const std::vector<std::size_t>& indices) {
    std::string out;
    for (const std::size_t index : indices) {
        if (!out.empty()) out.push_back('.');
        out += std::to_string(index);
    }
    return out;
}

void log_catalog_exception(std::string_view group, std::string_view path, const char* detail) {
    FB2K_console_formatter() << "Mouse Gesture Host: command provider failed in \""
                             << group.data() << "\" at \"" << path.data() << "\": " << detail;
}

void walk_menu_tree(menu_tree_item& parent, const std::string& prefix, const std::string& group,
                    std::vector<std::size_t>& indices, ResolvedCatalog& out) {
    const std::size_t count = parent.childCount();
    for (std::size_t i = 0; i < count; ++i) {
        indices.push_back(i);
        try {
            menu_tree_item::ptr child = parent.childAt(i);
            if (!child.is_valid()) {
                indices.pop_back();
                continue;
            }
            const auto type = child->type();
            if (type == menu_tree_item::itemSeparator) {
                indices.pop_back();
                continue;
            }

            const char* raw_name = child->name();
            const std::string name = raw_name != nullptr ? strip_mnemonics(raw_name) : std::string();
            if (name.empty()) {
                indices.pop_back();
                continue;
            }
            const std::string path = prefix.empty() ? name : prefix + "/" + name;

            if (type == menu_tree_item::itemSubmenu) {
                walk_menu_tree(*child, path, group, indices, out);
            } else if (type == menu_tree_item::itemCommand) {
                ResolvedEntry resolved;
                resolved.path = path;
                resolved.group = group;
                resolved.node_path = format_node_path(indices);

                const GUID command_guid = child->commandGuid();
                if (command_guid != pfc::guid_null) {
                    resolved.command_guid = pfc::print_guid(command_guid).c_str();
                }
                const GUID subcommand_guid = child->subCommandGuid();
                if (subcommand_guid != pfc::guid_null) {
                    resolved.subcommand_guid = pfc::print_guid(subcommand_guid).c_str();
                }
                resolved.item = std::move(child);
                out.entries.push_back(std::move(resolved));
            }
        } catch (const std::exception& e) {
            log_catalog_exception(group, prefix, e.what());
        } catch (...) {
            log_catalog_exception(group, prefix, "unknown exception");
        }
        indices.pop_back();
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

ResolvedCatalog list_main_menu() {
    ResolvedCatalog out;
    for (std::size_t root_index = 0; root_index < std::size(kMainMenuRoots); ++root_index) {
        const auto& root = kMainMenuRoots[root_index];
        try {
            mainmenu_manager_v2::ptr manager = mainmenu_manager_v2::tryGet();
            if (!manager.is_valid()) {
                out.error = "mainmenu_manager_v2 is unavailable";
                continue;
            }
            manager->instantiate(*root.guid);
            menu_tree_item::ptr tree = manager->generate_menu(mainmenu_manager::flag_view_full);
            if (!tree.is_valid()) {
                out.error = "main menu returned no menu tree";
                continue;
            }

            out.owners.push_back(manager);
            std::vector<std::size_t> indices{ root_index };
            walk_menu_tree(*tree, root.label, "main", indices, out);
        } catch (const std::exception& e) {
            log_catalog_exception("main", root.label, e.what());
            out.error = e.what();
        } catch (...) {
            log_catalog_exception("main", root.label, "unknown exception");
            out.error = "unknown main menu provider exception";
        }
    }
    return out;
}

ResolvedCatalog list_context_menu(const std::string& context_group,
                                  const std::string& identity_group) {
    ResolvedCatalog out;
    try {
        contextmenu_manager_v2::ptr manager = contextmenu_manager_v2::tryGet();
        if (!manager.is_valid()) {
            out.error = "contextmenu_manager_v2 is unavailable";
            return out;
        }
        if (!init_context(*manager, context_group)) return out;

        menu_tree_item::ptr tree = manager->build_menu();
        if (!tree.is_valid()) {
            out.error = "context menu returned no menu tree";
            return out;
        }

        out.owners.push_back(manager);
        std::vector<std::size_t> indices{ 0 };
        walk_menu_tree(*tree, std::string(), identity_group, indices, out);
    } catch (const std::exception& e) {
        log_catalog_exception(identity_group, "", e.what());
        out.error = e.what();
    } catch (...) {
        log_catalog_exception(identity_group, "", "unknown exception");
        out.error = "unknown context menu provider exception";
    }
    return out;
}

std::string effective_context_group(const GestureBinding& binding) {
    if (binding.place == "playlist") return "context_playlist";
    if (binding.place == "now_playing") return "context_now_playing";
    return binding.group;
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
    ResolvedCatalog resolved = group_id == "main"
        ? list_main_menu()
        : list_context_menu(std::string(group_id), std::string(group_id));
    sort_command_catalog(resolved.entries);

    std::vector<CatalogEntry> out;
    out.reserve(resolved.entries.size());
    for (const auto& entry : resolved.entries) {
        out.push_back(static_cast<const CatalogEntry&>(entry));
    }
    return out;
}

bool execute_binding(const GestureBinding& binding, std::string* diagnostic) {
    if (diagnostic != nullptr) diagnostic->clear();
    if (binding.command_path.empty()) {
        if (diagnostic != nullptr) *diagnostic = "empty command path";
        return false;
    }

    ResolvedCatalog resolved = binding.group == "main"
        ? list_main_menu()
        : list_context_menu(effective_context_group(binding), binding.group);
    const CommandMatch match = resolve_command(binding, resolved.entries);
    if (match.status != CommandResolution::found) {
        if (diagnostic != nullptr) {
            *diagnostic = match.message;
            if (!resolved.error.empty()) *diagnostic += "; " + resolved.error;
        }
        return false;
    }

    try {
        resolved.entries[match.index].item->execute(service_ptr());
    } catch (const std::exception& e) {
        if (diagnostic != nullptr) *diagnostic = std::string("command execution failed: ") + e.what();
        return false;
    } catch (...) {
        if (diagnostic != nullptr) *diagnostic = "command execution failed: unknown exception";
        return false;
    }
    return true;
}

} // namespace mgh
