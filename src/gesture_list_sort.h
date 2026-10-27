#pragma once

#include "command_identity.h"

namespace mgh {

enum class GestureSortColumn : std::uint8_t {
    none,
    gesture,
    command,
    place,
};

enum class GestureSortDirection : std::uint8_t {
    ascending,
    descending,
};

struct GestureSortState {
    GestureSortColumn column = GestureSortColumn::none;
    GestureSortDirection direction = GestureSortDirection::ascending;

    bool operator==(const GestureSortState&) const = default;
};

//! Отображаемая строка и уже вычисленные ключи. Компаратор не разбирает жесты
//! и не конвертирует UTF-8/UTF-16 на каждом сравнении.
struct GestureListRow {
    bool is_header = false;
    int binding_index = -1;
    std::wstring header;
    std::wstring gesture;
    std::wstring command;
    std::wstring place;

    GestureCategory gesture_category = GestureCategory::directional;
    int first_token_rank = 0;
    std::size_t gesture_token_count = 0;
    std::string canonical_gesture;
    std::string group_id;
    std::string command_path;
    std::string place_id;
};

//! Заполняет только ключи; отображаемые подписи подготавливает Win32 UI.
void prepare_gesture_list_row(GestureListRow& row, const GestureBinding& binding);

GestureSortColumn gesture_sort_column_from_index(int column_index);
GestureSortState next_gesture_sort_state(GestureSortState current, GestureSortColumn column);

//! Трёхстороннее сравнение по предрасчётным ключам, без direction и UI state.
int compare_gesture_list_rows(const GestureListRow& left, const GestureListRow& right,
                              GestureSortColumn column);

//! Возвращает отдельное представление; stable_sort не использует binding_index
//! как скрытый tie-break. В grouped mode категории идут в стабильном порядке,
//! а выбранная сортировка применяется внутри каждой группы.
std::vector<GestureListRow> make_gesture_list_view(const std::vector<GestureListRow>& manual_rows,
                                                   GestureSortState sort,
                                                   bool group_by_category = false);

int gesture_row_for_binding(const std::vector<GestureListRow>& rows, int binding_index);

enum class GestureEditorMode : std::uint8_t {
    idle,
    new_binding,
    edit,
};

struct GestureEditorState {
    GestureEditorMode mode = GestureEditorMode::idle;
    //! Эфемерный ID в пределах открытой страницы, не persisted config field.
    int binding_index = -1;
    GestureBinding draft;
    GestureBinding baseline;
    bool dirty = false;
};

GestureEditorState make_new_editor_state(const std::string& group_id = "main",
                                         const std::string& place_id = "main_window");
GestureEditorState make_edit_editor_state(const GestureBinding& binding, int binding_index);
void refresh_editor_dirty(GestureEditorState& editor);
void discard_editor_changes(GestureEditorState& editor);

//! Дубликат определяется пользовательскими полями, а не служебным node_path.
bool has_exact_binding_duplicate(const std::vector<GestureBinding>& bindings,
                                 const GestureBinding& candidate, int ignore_index = -1);

enum class GestureEditorCommandState : std::uint8_t {
    none,
    valid,
    unavailable,
    conflict,
};

struct GestureEditorCommandStatus {
    GestureEditorCommandState state = GestureEditorCommandState::none;
    int catalog_index = -1;
    std::string message = "Select command...";

    bool is_selectable() const { return state == GestureEditorCommandState::valid && catalog_index >= 0; }
};

//! Обновляет только draft; индекс -1 означает явно пустой выбор команды.
void select_editor_command(GestureBinding& draft, const std::vector<CatalogEntry>& catalog,
                           int catalog_index);

//! После смены группы сохраняет identity только при однозначном совпадении.
//! Возвращает индекс команды в catalog либо -1; первый элемент не выбирается.
int change_editor_group(GestureBinding& draft, const std::string& group_id,
                        const std::vector<CatalogEntry>& catalog);

//! Единые правила Add/Update: synthetic/conflict/unavailable строки не executable.
GestureEditorCommandStatus validate_editor_command(const GestureBinding& draft,
                                                    const std::vector<CatalogEntry>& catalog,
                                                    int catalog_index);

} // namespace mgh
