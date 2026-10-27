#include "gesture_list_sort.h"

#include <algorithm>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mgh {
namespace {

template<typename T>
int compare_value(const T& left, const T& right) {
    if (left < right) return -1;
    if (right < left) return 1;
    return 0;
}

int compare_text(const std::wstring& left, const std::wstring& right) {
#ifdef _WIN32
    const int result = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                                      left.c_str(), -1, right.c_str(), -1);
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
#else
    // Локальные Linux tests используют ASCII labels; Windows build всегда идёт
    // через CompareStringW и учёт локали пользователя.
    const std::size_t common = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t i = 0; i < common; ++i) {
        const auto lower = [](wchar_t c) {
            return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        };
        const wchar_t left_char = lower(left[i]);
        const wchar_t right_char = lower(right[i]);
        if (left_char < right_char) return -1;
        if (left_char > right_char) return 1;
    }
    return compare_value(left.size(), right.size());
#endif
}

void clear_command_identity(GestureBinding& binding) {
    binding.command_path.clear();
    binding.command_guid.clear();
    binding.subcommand_guid.clear();
    binding.node_path.clear();
}

bool same_catalog_entry(const CatalogEntry& left, const CatalogEntry& right) {
    return left.path == right.path && left.group == right.group &&
           left.command_guid == right.command_guid && left.subcommand_guid == right.subcommand_guid &&
           left.node_path == right.node_path;
}

std::vector<CatalogEntry> selectable_entries(const std::vector<CatalogEntry>& catalog) {
    std::vector<CatalogEntry> result;
    result.reserve(catalog.size());
    for (const auto& entry : catalog) {
        if (entry.selectable) result.push_back(entry);
    }
    return result;
}

} // namespace

void prepare_gesture_list_row(GestureListRow& row, const GestureBinding& binding) {
    row.canonical_gesture = binding.gesture;
    row.group_id = binding.group;
    row.command_path = binding.command_path;
    row.place_id = binding.place;
    row.gesture_category = categorize_gesture(binding.gesture);

    std::vector<Token> tokens;
    if (!parse_gesture(binding.gesture, tokens) || tokens.empty()) {
        row.first_token_rank = (std::numeric_limits<int>::max)();
        row.gesture_token_count = 0;
        return;
    }
    row.first_token_rank = static_cast<int>(tokens.front());
    row.gesture_token_count = tokens.size();
}

GestureSortColumn gesture_sort_column_from_index(int column_index) {
    switch (column_index) {
    case 0: return GestureSortColumn::gesture;
    case 1: return GestureSortColumn::command;
    case 2: return GestureSortColumn::place;
    default: return GestureSortColumn::none;
    }
}

GestureSortState next_gesture_sort_state(GestureSortState current, GestureSortColumn column) {
    if (column == GestureSortColumn::none) return {};
    if (current.column != column) return { column, GestureSortDirection::ascending };
    if (current.direction == GestureSortDirection::ascending) {
        return { column, GestureSortDirection::descending };
    }
    return {};
}

int compare_gesture_list_rows(const GestureListRow& left, const GestureListRow& right,
                              GestureSortColumn column) {
    switch (column) {
    case GestureSortColumn::gesture: {
        int result = compare_value(static_cast<int>(left.gesture_category),
                                   static_cast<int>(right.gesture_category));
        if (result != 0) return result;
        result = compare_value(left.first_token_rank, right.first_token_rank);
        if (result != 0) return result;
        result = compare_value(left.gesture_token_count, right.gesture_token_count);
        if (result != 0) return result;
        return compare_value(left.canonical_gesture, right.canonical_gesture);
    }
    case GestureSortColumn::command: {
        int result = compare_text(left.command, right.command);
        if (result != 0) return result;
        result = compare_value(left.group_id, right.group_id);
        if (result != 0) return result;
        return compare_value(left.command_path, right.command_path);
    }
    case GestureSortColumn::place: {
        const int result = compare_text(left.place, right.place);
        if (result != 0) return result;
        return compare_value(left.place_id, right.place_id);
    }
    case GestureSortColumn::none:
        return 0;
    }
    return 0;
}

std::vector<GestureListRow> make_gesture_list_view(const std::vector<GestureListRow>& manual_rows,
                                                   GestureSortState sort, bool group_by_category) {
    std::vector<GestureListRow> view = manual_rows;
    if (sort.column != GestureSortColumn::none) {
        std::stable_sort(view.begin(), view.end(), [sort](const GestureListRow& left,
                                                         const GestureListRow& right) {
            const int result = compare_gesture_list_rows(left, right, sort.column);
            return sort.direction == GestureSortDirection::descending ? result > 0 : result < 0;
        });
    }
    if (!group_by_category) return view;

    constexpr GestureCategory kCategoryOrder[] = {
        GestureCategory::directional,
        GestureCategory::wheel_up,
        GestureCategory::wheel_down,
        GestureCategory::rocker,
    };

    std::vector<GestureListRow> grouped;
    grouped.reserve(view.size() + 4);
    for (const GestureCategory category : kCategoryOrder) {
        const auto first = std::find_if(view.begin(), view.end(), [category](const GestureListRow& row) {
            return row.gesture_category == category;
        });
        if (first == view.end()) continue;

        GestureListRow header;
        header.is_header = true;
        header.gesture_category = category;
        grouped.push_back(std::move(header));
        for (const auto& row : view) {
            if (row.gesture_category == category) grouped.push_back(row);
        }
    }
    return grouped;
}

int gesture_row_for_binding(const std::vector<GestureListRow>& rows, int binding_index) {
    if (binding_index < 0) return -1;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].binding_index == binding_index) return static_cast<int>(i);
    }
    return -1;
}

void select_editor_command(GestureBinding& draft, const std::vector<CatalogEntry>& catalog,
                           int catalog_index) {
    if (catalog_index < 0 || catalog_index >= static_cast<int>(catalog.size())) {
        clear_command_identity(draft);
        return;
    }

    const auto& entry = catalog[static_cast<std::size_t>(catalog_index)];
    draft.group = entry.group;
    assign_command_identity(draft, entry);
}

int change_editor_group(GestureBinding& draft, const std::string& group_id,
                        const std::vector<CatalogEntry>& catalog) {
    draft.group = group_id;
    if (draft.command_path.empty()) return -1;

    const CommandMatch match = resolve_command(draft, catalog);
    if (match.status != CommandResolution::found) {
        clear_command_identity(draft);
        return -1;
    }

    const auto& entry = catalog[match.index];
    if (!entry.selectable) {
        clear_command_identity(draft);
        return -1;
    }
    assign_command_identity(draft, entry);
    return static_cast<int>(match.index);
}

GestureEditorCommandStatus validate_editor_command(const GestureBinding& draft,
                                                    const std::vector<CatalogEntry>& catalog,
                                                    int catalog_index) {
    GestureEditorCommandStatus status;
    status.catalog_index = catalog_index;
    if (catalog_index < 0 || catalog_index >= static_cast<int>(catalog.size())) {
        status.message = "Select command...";
        return status;
    }

    const auto& selected = catalog[static_cast<std::size_t>(catalog_index)];
    const auto real_entries = selectable_entries(catalog);
    const CommandMatch match = resolve_command(draft, real_entries);

    if (!selected.selectable) {
        status.state = match.status == CommandResolution::ambiguous
            ? GestureEditorCommandState::conflict
            : GestureEditorCommandState::unavailable;
        status.message = match.message.empty()
            ? "Saved command requires explicit reselection."
            : match.message;
        return status;
    }

    if (match.status == CommandResolution::ambiguous) {
        status.state = GestureEditorCommandState::conflict;
        status.message = match.message;
        return status;
    }
    if (match.status != CommandResolution::found) {
        status.state = GestureEditorCommandState::unavailable;
        status.message = match.message.empty() ? "Command is unavailable." : match.message;
        return status;
    }
    if (!same_catalog_entry(selected, real_entries[match.index])) {
        status.state = GestureEditorCommandState::conflict;
        status.message = "Selected row does not resolve to the same command identity.";
        return status;
    }

    status.state = GestureEditorCommandState::valid;
    status.message.clear();
    return status;
}

} // namespace mgh
