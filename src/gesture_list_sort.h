#pragma once

#include "gesture_config.h"

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

} // namespace mgh
