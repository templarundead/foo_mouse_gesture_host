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

int first_binding_row(const std::vector<GestureListRow>& rows) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!rows[static_cast<std::size_t>(i)].is_header) return static_cast<int>(i);
    }
    return -1;
}

int last_binding_row(const std::vector<GestureListRow>& rows) {
    for (std::size_t i = rows.size(); i > 0; --i) {
        if (!rows[i - 1].is_header) return static_cast<int>(i - 1);
    }
    return -1;
}

int adjacent_binding_row(const std::vector<GestureListRow>& rows, int current_row, int direction) {
    const int count = static_cast<int>(rows.size());
    if (count == 0) return -1;
    if (current_row < 0) return first_binding_row(rows);
    if (current_row >= count) return direction < 0 ? last_binding_row(rows) : -1;

    for (int row = current_row + direction; row >= 0 && row < count; row += direction) {
        if (!rows[static_cast<std::size_t>(row)].is_header) return row;
    }
    return rows[static_cast<std::size_t>(current_row)].is_header ? -1 : current_row;
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

int gesture_navigation_row(const std::vector<GestureListRow>& rows, int current_row,
                           GestureRowNavigation navigation, int page_rows) {
    switch (navigation) {
    case GestureRowNavigation::first:
        return first_binding_row(rows);
    case GestureRowNavigation::last:
        return last_binding_row(rows);
    case GestureRowNavigation::previous:
        return adjacent_binding_row(rows, current_row, -1);
    case GestureRowNavigation::next:
        return adjacent_binding_row(rows, current_row, 1);
    case GestureRowNavigation::page_previous:
    case GestureRowNavigation::page_next:
        break;
    }

    const int direction = navigation == GestureRowNavigation::page_previous ? -1 : 1;
    int target = current_row;
    int steps = page_rows > 0 ? page_rows : 1;
    if (target < 0 || target >= static_cast<int>(rows.size()) ||
        rows[static_cast<std::size_t>(target)].is_header) {
        target = adjacent_binding_row(rows, target, direction);
        if (target < 0) return -1;
        --steps;
    }

    for (int step = 0; step < steps; ++step) {
        const int next = adjacent_binding_row(rows, target, direction);
        if (next < 0 || next == target) break;
        target = next;
    }
    return target;
}

bool can_move_gesture_bindings(GestureSortState sort, bool group_by_category) {
    return sort.column == GestureSortColumn::none && !group_by_category;
}

bool move_gesture_binding(std::vector<GestureBinding>& bindings, int binding_index, int delta,
                          GestureSortState sort, bool group_by_category) {
    if (!can_move_gesture_bindings(sort, group_by_category) || (delta != -1 && delta != 1)) {
        return false;
    }

    const int count = static_cast<int>(bindings.size());
    if (binding_index < 0 || binding_index >= count) return false;
    const int target = binding_index + delta;
    if (target < 0 || target >= count) return false;

    std::swap(bindings[static_cast<std::size_t>(binding_index)],
              bindings[static_cast<std::size_t>(target)]);
    return true;
}

GestureEditorState make_new_editor_state(const std::string& group_id, const std::string& place_id) {
    GestureEditorState editor;
    editor.mode = GestureEditorMode::new_binding;
    editor.draft.group = group_id;
    editor.draft.place = place_id;
    editor.baseline = editor.draft;
    return editor;
}

GestureEditorState make_edit_editor_state(const GestureBinding& binding, int binding_index) {
    GestureEditorState editor;
    editor.mode = GestureEditorMode::edit;
    editor.binding_index = binding_index;
    editor.draft = binding;
    editor.baseline = binding;
    return editor;
}

void refresh_editor_dirty(GestureEditorState& editor) {
    editor.dirty = editor.mode != GestureEditorMode::idle && editor.draft != editor.baseline;
}

void discard_editor_changes(GestureEditorState& editor) {
    editor.draft = editor.baseline;
    editor.dirty = false;
}

bool has_exact_binding_duplicate(const std::vector<GestureBinding>& bindings,
                                 const GestureBinding& candidate, int ignore_index) {
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        if (static_cast<int>(i) == ignore_index) continue;
        const auto& binding = bindings[i];
        if (binding.gesture == candidate.gesture && binding.group == candidate.group &&
            binding.command_path == candidate.command_path && binding.place == candidate.place) {
            return true;
        }
    }
    return false;
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
    if (draft.command_path.empty()) {
        clear_command_identity(draft);
        return -1;
    }

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
