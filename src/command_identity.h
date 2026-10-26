#pragma once

#include "gesture_config.h"

namespace mgh {

//! Одна команда, доступная для привязки к жесту.
struct CatalogEntry {
    std::string path;
    std::string group;
    std::string command_guid;
    std::string subcommand_guid;
    std::string node_path;

    //! Synthetic unavailable/conflict row в Preferences не является исполняемой командой.
    bool selectable = true;
};

enum class CommandResolution {
    found,
    unavailable,
    ambiguous,
    invalid,
};

struct CommandMatch {
    CommandResolution status = CommandResolution::unavailable;
    std::size_t index = 0;
    std::string message;
};

inline void assign_command_identity(GestureBinding& binding, const CatalogEntry& entry) {
    binding.command_path = entry.path;
    binding.command_guid = entry.command_guid;
    binding.subcommand_guid = entry.subcommand_guid;
    binding.node_path = entry.node_path;
}

//! Разрешение без leaf fallback. Работает и с CatalogEntry, и с SDK-обёрткой,
//! у которой есть те же публичные строковые поля.
template<typename Range>
CommandMatch resolve_command(const GestureBinding& binding, const Range& entries) {
    CommandMatch result;
    if (!is_valid_command_group(binding.group) || !is_valid_command_place(binding.place) ||
        binding.command_path.empty()) {
        result.status = CommandResolution::invalid;
        result.message = "invalid binding group, place, or empty command path";
        return result;
    }

    std::vector<std::size_t> matches;
    const auto finish = [&result, &matches](CommandResolution ambiguous_status) {
        if (matches.size() == 1) {
            result.status = CommandResolution::found;
            result.index = matches.front();
            return true;
        }
        result.status = matches.empty() ? CommandResolution::unavailable : ambiguous_status;
        if (!matches.empty()) {
            result.message = "ambiguous command identity: " + std::to_string(matches.size()) + " matches";
        } else {
            result.message = "command is unavailable in the current menu";
        }
        return false;
    };

    if (!binding.command_guid.empty()) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if (entry.group == binding.group && entry.command_guid == binding.command_guid &&
                entry.subcommand_guid == binding.subcommand_guid) {
                matches.push_back(i);
            }
        }
        // GUID - устойчивая идентичность: изменение display name не ломает привязку.
        finish(CommandResolution::ambiguous);
        return result;
    }

    if (!binding.node_path.empty()) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if (entry.group == binding.group && entry.node_path == binding.node_path) {
                matches.push_back(i);
            }
        }
        if (finish(CommandResolution::ambiguous)) {
            const auto& entry = entries[result.index];
            if (entry.path != binding.command_path) {
                result.status = CommandResolution::unavailable;
                result.message = "structural key now points to a different command label";
            }
        }
        return result;
    }

    // Legacy v1/v2 без identity: только единственный полный путь, никогда не leaf.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.group == binding.group && entry.path == binding.command_path) {
            matches.push_back(i);
        }
    }
    finish(CommandResolution::ambiguous);
    return result;
}

} // namespace mgh
