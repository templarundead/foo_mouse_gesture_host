#include "preferences_page.h"

#include "command_catalog.h"
#include "gesture_engine.h"
#include "gesture_list_sort.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cwchar>
#include <functional>
#include <optional>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace mgh {
namespace {

constexpr GUID kPreferencesPageGuid = { 0x6f0c4a52, 0x8d31, 0x4c0e, { 0x9b, 0x7e, 0x2a, 0x64, 0x11, 0xd8, 0x3f, 0x10 } };
constexpr wchar_t kPreferencesPageClass[] = L"foo_mouse_gesture_host_preferences";
constexpr UINT_PTR kCommandListSubclassId = 0x4d4748;
constexpr UINT_PTR kHelpButtonSubclassId = 0x4d4749;
constexpr UINT_PTR kHelpTrackingToolId = 1;

enum ControlId : int {
    kIdList = 1001,
    kIdDirUp,
    kIdDirDown,
    kIdDirLeft,
    kIdDirRight,
    kIdDirUpLeft,
    kIdDirUpRight,
    kIdDirDownLeft,
    kIdDirDownRight,
    kIdDirBackspace,
    kIdWheelUp,
    kIdWheelDown,
    kIdRockerRightLeft,
    kIdRockerLeftRight,
    kIdGestureEdit,
    kIdGroupCombo,
    kIdCommandCombo,
    kIdPlaceCombo,
    kIdNew,
    kIdAdd,
    kIdUpdate,
    kIdDelete,
    kIdMoveUp,
    kIdMoveDown,
    kIdCommandChooseAnother,
    kIdCommandRetry,
    kIdCommandCopyDetails,
    kIdStrokeEdit,
    kIdStrokeSpin,
    kIdDisable,
    kIdAlphabet,
    kIdFirstOnly,
    kIdHelp,
    kIdGroupByCategory
};

enum class EditorActionType : std::uint8_t {
    none,
    select,
    add,
    new_binding,
    reset,
    apply,
    remove,
    move,
};

struct EditorPendingAction {
    EditorActionType type = EditorActionType::none;
    int binding_index = -1;
    int delta = 0;
};

enum class EditorGuardDecision : std::uint8_t {
    save,
    discard,
    cancel,
};

const wchar_t* editor_action_label(EditorActionType type) {
    switch (type) {
    case EditorActionType::select: return L"select binding";
    case EditorActionType::add: return L"Add";
    case EditorActionType::new_binding: return L"New";
    case EditorActionType::reset: return L"Reset";
    case EditorActionType::apply: return L"Apply";
    case EditorActionType::remove: return L"Delete";
    case EditorActionType::move: return L"Move";
    case EditorActionType::none: break;
    }
    return L"action";
}

// Preferences живёт в UI-потоке. Session переживает уничтожение страницы,
// но не restart/forced shutdown/crash и не меняет persisted config.
std::optional<GestureEditorState> g_editor_draft_session;

//! Идентификаторы групп ListView (заголовки-разделители в таблице).
enum ListGroupId : int {
    kGroupGesture = 1,
    kGroupWheelUp,
    kGroupWheelDown,
    kGroupRocker
};

constexpr int kMinStrokeLength = 1;
constexpr int kMaxStrokeLength = 100;

int get_window_dpi(HWND hwnd) {
    // GetDpiForWindow появился в Windows 10 1607 - загружаем динамически,
    // чтобы не требовать свежий SDK при сборке.
    using get_dpi_for_window_t = UINT(WINAPI*)(HWND);
    static const auto get_dpi_for_window = reinterpret_cast<get_dpi_for_window_t>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));

    if (get_dpi_for_window != nullptr) {
        const UINT dpi = get_dpi_for_window(hwnd);
        if (dpi != 0) return static_cast<int>(dpi);
    }

    HDC dc = GetDC(hwnd);
    int dpi = 96;
    if (dc != nullptr) {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(hwnd, dc);
    }
    return dpi > 0 ? dpi : 96;
}

std::wstring read_window_text(HWND wnd) {
    const int length = GetWindowTextLengthW(wnd);
    if (length <= 0) return std::wstring();

    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(wnd, buffer.data(), length + 1);
    buffer.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return buffer;
}

std::wstring arrow(const char* utf8) {
    return wide_from_utf8(utf8);
}

constexpr wchar_t kPreferencesHelp[] =
    L"1. Build a gesture using the buttons on the left.\r\n"
    L"2. Select a command and click Add New, then Apply.\r\n"
    L"R+L: hold RMB and click LMB.\r\n"
    L"L+R: hold LMB and click RMB.";
constexpr wchar_t kStrokeHelp[] = L"Stroke Length: recognition threshold from 1 to 100.";

const wchar_t* gesture_category_title(GestureCategory category) {
    switch (category) {
    case GestureCategory::wheel_up: return L"Wheel Up";
    case GestureCategory::wheel_down: return L"Wheel Down";
    case GestureCategory::rocker: return L"Rocker";
    default: return L"Gesture";
    }
}

const wchar_t* gesture_category_chip(GestureCategory category) {
    switch (category) {
    case GestureCategory::wheel_up: return L"W↑";
    case GestureCategory::wheel_down: return L"W↓";
    case GestureCategory::rocker: return L"R↔";
    default: return L"Dir";
    }
}

BOOL CALLBACK invalidate_child_proc(HWND child, LPARAM) {
    InvalidateRect(child, nullptr, TRUE);
    return TRUE;
}

} // namespace

// ---------------------------------------------------------------------------
//  Таблица привязок
// ---------------------------------------------------------------------------
//
//  Свой контрол вместо SysListView32. Причина: в тёмной теме foobar2000
//  встроенная отрисовка ListView использует цвета темы, а при
//  LVS_OWNERDRAWFIXED хост вообще не доставляет WM_DRAWITEM странице -
//  строки остаются невидимыми. Здесь всё рисуется самим контролом, поэтому
//  вид одинаков в светлой и тёмной теме и не зависит от comctl32.

class GestureListControl {
public:
    using select_handler = std::function<void(int binding_index)>;
    using select_guard_handler = std::function<bool(int binding_index)>;
    using sort_handler = std::function<void()>;

    static const wchar_t* class_name() { return L"foo_mouse_gesture_host_list"; }

    static void register_class() {
        static bool registered = false;
        if (registered) return;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &GestureListControl::window_proc_static;
        wc.hInstance = core_api::get_my_instance();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = class_name();
        RegisterClassExW(&wc);

        registered = true;
    }

    HWND create(HWND parent, int id) {
        register_class();
        return CreateWindowExW(WS_EX_CLIENTEDGE, class_name(), L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
                               0, 0, 100, 100, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               core_api::get_my_instance(), this);
    }

    void set_fonts(HFONT font, HFONT header_font, int dpi) {
        font_ = font;
        header_font_ = header_font;
        dpi_ = dpi;
        recalculate_row_heights();
    }

    void set_colors(COLORREF background, COLORREF text, COLORREF muted,
                    COLORREF selected_background, COLORREF selected_text, COLORREF header_background) {
        background_ = background;
        text_ = text;
        muted_ = muted;
        selected_background_ = selected_background;
        selected_text_ = selected_text;
        header_background_ = header_background;
        invalidate();
    }

    void set_rows(std::vector<GestureListRow>* rows) {
        rows_ = rows;
        selected_ = -1;
        scroll_ = 0;
        update_scroll_info();
        invalidate();
    }

    void set_select_handler(select_handler handler) { on_select_ = std::move(handler); }
    void set_select_guard_handler(select_guard_handler handler) { on_select_guard_ = std::move(handler); }
    void set_sort_handler(sort_handler handler) { on_sort_ = std::move(handler); }
    void set_owner_service(service_base* owner) { owner_service_ = owner; }

    void set_category_chips(bool show) {
        if (show_category_chips_ == show) return;
        show_category_chips_ = show;
        const int minimum = min_column_width(0);
        if (columns_[0] < minimum) columns_[0] = minimum;
        update_scroll_info();
        invalidate();
    }

    GestureSortState sort_state() const { return sort_state_; }
    void set_sort_state(GestureSortState state) {
        sort_state_ = state;
        invalidate();
    }

    int selected_binding() const {
        if (rows_ == nullptr || selected_ < 0 || selected_ >= static_cast<int>(rows_->size())) return -1;
        return (*rows_)[static_cast<std::size_t>(selected_)].binding_index;
    }

    void select_binding(int binding_index) {
        if (rows_ == nullptr) return;

        const int row = gesture_row_for_binding(*rows_, binding_index);
        if (row < 0) return;
        selected_ = row;
        scroll_to_row(selected_);
        invalidate();
    }

    void clear_selection() {
        selected_ = -1;
        invalidate();
    }

    //! Пересчитать прокрутку и перерисовать (после изменения размера/строк).
    void refresh() {
        update_scroll_info();
        invalidate();
    }

private:
    static LRESULT CALLBACK window_proc_static(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* self = reinterpret_cast<GestureListControl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self != nullptr) self->hwnd_ = hwnd;
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        auto* self = reinterpret_cast<GestureListControl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr) {
            if (msg == WM_LBUTTONDOWN || msg == WM_KEYDOWN) {
                // Только эти сообщения могут открыть guard. На WM_NCDESTROY
                // нельзя поднимать refcount уже разрушаемого owner.
                service_ptr_t<service_base> keep_alive(self->owner_service_);
                return self->window_proc(msg, wparam, lparam);
            }
            return self->window_proc(msg, wparam, lparam);
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    int scale(int value) const { return MulDiv(value, dpi_, 96); }

    void invalidate() {
        if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    int row_height(bool header) const {
        return header ? header_height_ : row_height_;
    }

    void recalculate_row_heights() {
        HDC dc = hwnd_ != nullptr ? GetDC(hwnd_) : nullptr;
        if (dc == nullptr) return;

        HGDIOBJ old_font = SelectObject(dc, font_);
        TEXTMETRICW metrics = {};
        if (GetTextMetricsW(dc, &metrics)) {
            row_height_ = static_cast<int>(metrics.tmHeight) + scale(8);
            column_header_height_ = static_cast<int>(metrics.tmHeight) + scale(6);
        }

        SelectObject(dc, header_font_ != nullptr ? header_font_ : font_);
        if (GetTextMetricsW(dc, &metrics)) header_height_ = static_cast<int>(metrics.tmHeight) + scale(12);

        SelectObject(dc, old_font);
        ReleaseDC(hwnd_, dc);

        if (row_height_ < scale(18)) row_height_ = scale(18);
        if (header_height_ < scale(22)) header_height_ = scale(22);
        if (column_header_height_ < scale(20)) column_header_height_ = scale(20);

        update_scroll_info();
    }

    int content_height() const {
        if (rows_ == nullptr) return 0;

        int total = 0;
        for (const auto& row : *rows_) total += row_height(row.is_header);
        return total;
    }

    int columns_width() const {
        return columns_[0] + columns_[1] + columns_[2];
    }

    //! Пропорциональная ширина по умолчанию. После ручного изменения ширины
    //! колонки больше не пересчитываются, как у обычного ListView.
    void layout_columns() {
        if (columns_customized_ || hwnd_ == nullptr) return;

        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int width = client.right - client.left;
        if (width <= 0) return;

        const int min_gesture = scale(show_category_chips_ ? 92 : 60);
        const int min_command = scale(160);
        const int min_place = scale(80);

        int gesture = width * 22 / 100;
        int command = width * 58 / 100;
        int place = width - gesture - command;

        if (place < min_place) {
            place = min_place;
            command = width - gesture - place;
        }
        if (command < min_command) {
            command = min_command;
            gesture = width - command - place;
        }
        if (gesture < min_gesture) gesture = min_gesture;

        columns_[0] = gesture;
        columns_[1] = command;
        columns_[2] = place;
    }

    void update_scroll_info() {
        if (hwnd_ == nullptr) return;

        layout_columns();

        RECT client = {};
        GetClientRect(hwnd_, &client);
        int page = client.bottom - client.top - column_header_height_;
        if (page < 0) page = 0;
        const int total = content_height();

        SCROLLINFO info = {};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = total > 0 ? total - 1 : 0;
        info.nPage = static_cast<UINT>(page);
        info.nPos = scroll_;
        SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);

        // Вертикальная полоса уменьшает клиентскую ширину. Учитываем это
        // перед расчётом горизонтального диапазона и положения колонок.
        layout_columns();
        GetClientRect(hwnd_, &client);
        const int page_width = client.right - client.left > 0 ? client.right - client.left : 0;
        const int total_width = columns_width();

        info = {};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = total_width > 0 ? total_width - 1 : 0;
        info.nPage = static_cast<UINT>(page_width);
        info.nPos = scroll_x_;
        SetScrollInfo(hwnd_, SB_HORZ, &info, TRUE);

        clamp_scroll();
    }

    void clamp_scroll() {
        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int page = client.bottom - client.top - column_header_height_;
        const int max_scroll = content_height() - page;
        if (scroll_ > max_scroll) scroll_ = max_scroll;
        if (scroll_ < 0) scroll_ = 0;

        const int page_width = client.right - client.left;
        const int max_scroll_x = columns_width() - page_width;
        if (scroll_x_ > max_scroll_x) scroll_x_ = max_scroll_x;
        if (scroll_x_ < 0) scroll_x_ = 0;
    }

    void scroll_to(int position) {
        scroll_ = position;
        clamp_scroll();
        update_scroll_info();
        invalidate();
    }

    void scroll_horizontal_to(int position) {
        scroll_x_ = position;
        clamp_scroll();
        update_scroll_info();
        invalidate();
    }

    int column_divider_at(int x, int y) const {
        if (y < 0 || y >= column_header_height_) return -1;

        const int tolerance = scale(3);
        int divider = -scroll_x_;
        for (int column = 0; column < 3; ++column) {
            divider += columns_[column];
            if (x >= divider - tolerance && x <= divider + tolerance) return column;
        }
        return -1;
    }

    int column_header_at(int x, int y) const {
        if (y < 0 || y >= column_header_height_) return -1;

        int left = -scroll_x_;
        for (int column = 0; column < 3; ++column) {
            const int right = left + columns_[column];
            if (x >= left && x < right) return column;
            left = right;
        }
        return -1;
    }

    int min_column_width(int column) const {
        switch (column) {
        case 0: return scale(show_category_chips_ ? 92 : 60);
        case 1: return scale(160);
        default: return scale(80);
        }
    }

    void scroll_to_row(int row_index) {
        if (rows_ == nullptr || row_index < 0) return;

        int top = 0;
        for (int i = 0; i < row_index && i < static_cast<int>(rows_->size()); ++i) {
            top += row_height((*rows_)[static_cast<std::size_t>(i)].is_header);
        }
        const int height = row_height((*rows_)[static_cast<std::size_t>(row_index)].is_header);

        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int page = client.bottom - client.top - column_header_height_;

        if (top < scroll_) scroll_to(top);
        else if (top + height > scroll_ + page) scroll_to(top + height - page);
    }

    int row_at(int client_y) const {
        if (rows_ == nullptr || client_y < column_header_height_) return -1;

        int y = column_header_height_ - scroll_;
        for (std::size_t i = 0; i < rows_->size(); ++i) {
            const int height = row_height((*rows_)[i].is_header);
            if (client_y >= y && client_y < y + height) return static_cast<int>(i);
            y += height;
        }
        return -1;
    }

    void select_row(int index, bool notify) {
        if (rows_ == nullptr || index < 0 || index >= static_cast<int>(rows_->size())) return;
        if (index == selected_ || (*rows_)[static_cast<std::size_t>(index)].is_header) return;

        const int binding_index = (*rows_)[static_cast<std::size_t>(index)].binding_index;
        if (on_select_guard_ && !on_select_guard_(binding_index)) return;

        selected_ = index;
        invalidate();

        if (notify && on_select_) on_select_(binding_index);
    }

    void paint(HDC target, const RECT& client);
    void draw_column_header(HDC dc, const RECT& client);
    void draw_row(HDC dc, const RECT& row_rect, const GestureListRow& row, bool selected);

    LRESULT window_proc(UINT msg, WPARAM wparam, LPARAM lparam);

    HWND hwnd_ = nullptr;
    std::vector<GestureListRow>* rows_ = nullptr;

    HFONT font_ = nullptr;
    HFONT header_font_ = nullptr;
    int dpi_ = 96;

    int row_height_ = 22;
    int header_height_ = 26;
    int column_header_height_ = 22;

    int columns_[3] = { 160, 320, 120 };

    int scroll_ = 0;
    int scroll_x_ = 0;
    int selected_ = -1;
    bool focused_ = false;

    bool columns_customized_ = false;
    bool show_category_chips_ = false;
    int resizing_column_ = -1;
    int resize_anchor_x_ = 0;
    int resize_start_width_ = 0;
    GestureSortState sort_state_;

    COLORREF background_ = RGB(32, 32, 32);
    COLORREF text_ = RGB(230, 230, 230);
    COLORREF muted_ = RGB(150, 150, 150);
    COLORREF selected_background_ = RGB(0, 90, 158);
    COLORREF selected_text_ = RGB(255, 255, 255);
    COLORREF header_background_ = RGB(45, 45, 45);

    select_handler on_select_;
    select_guard_handler on_select_guard_;
    sort_handler on_sort_;
    service_base* owner_service_ = nullptr;
};

void GestureListControl::draw_row(HDC dc, const RECT& row_rect, const GestureListRow& row, bool selected) {
    RECT rc = row_rect;

    if (row.is_header) {
        HBRUSH brush = CreateSolidBrush(header_background_);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);

        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old_font = SelectObject(dc, header_font_ != nullptr ? header_font_ : font_);
        SetTextColor(dc, text_);

        SIZE extent = {};
        GetTextExtentPoint32W(dc, row.header.c_str(), static_cast<int>(row.header.size()), &extent);

        const int center = (rc.left + rc.right) / 2;
        const int text_left = center - extent.cx / 2;
        const int middle = (rc.top + rc.bottom) / 2;

        RECT text_rc = rc;
        text_rc.left = text_left;
        text_rc.right = text_left + extent.cx;
        DrawTextW(dc, row.header.c_str(), static_cast<int>(row.header.size()), &text_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Линии по бокам заголовка - как в group view ListView.
        HPEN pen = CreatePen(PS_SOLID, 1,
                             (GetRValue(header_background_) + GetGValue(header_background_) +
                              GetBValue(header_background_)) / 3 > 128
                                 ? RGB(180, 180, 180)
                                 : RGB(90, 90, 90));
        HGDIOBJ old_pen = SelectObject(dc, pen);

        const int gap = scale(8);
        if (text_left - gap > rc.left + scale(4)) {
            MoveToEx(dc, rc.left + scale(4), middle, nullptr);
            LineTo(dc, text_left - gap, middle);
        }
        if (text_left + extent.cx + gap < rc.right - scale(4)) {
            MoveToEx(dc, text_left + extent.cx + gap, middle, nullptr);
            LineTo(dc, rc.right - scale(4), middle);
        }

        SelectObject(dc, old_pen);
        DeleteObject(pen);
        SelectObject(dc, old_font);
        return;
    }

    HBRUSH brush = CreateSolidBrush(selected ? selected_background_ : background_);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, font_);

    const std::wstring* cells[3] = { &row.gesture, &row.command, &row.place };

    int left = rc.left - scroll_x_;
    for (int column = 0; column < 3; ++column) {
        RECT cell = rc;
        cell.left = left + scale(6);
        cell.right = left + columns_[column] - scale(4);
        left += columns_[column];

        if (cell.right <= cell.left) continue;

        if (column == 0 && show_category_chips_) {
            RECT chip = cell;
            chip.right = chip.left + scale(30);
            chip.top += scale(2);
            chip.bottom -= scale(2);
            if (chip.right > chip.left && chip.bottom > chip.top) {
                HBRUSH chip_brush = CreateSolidBrush(header_background_);
                FillRect(dc, &chip, chip_brush);
                DeleteObject(chip_brush);
                SetTextColor(dc, text_);
                DrawTextW(dc, gesture_category_chip(row.gesture_category), -1, &chip,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            cell.left += scale(34);
            if (cell.right <= cell.left) continue;
        }

        SetTextColor(dc, selected ? selected_text_ : (column == 2 ? muted_ : text_));
        DrawTextW(dc, cells[column]->c_str(), static_cast<int>(cells[column]->size()), &cell,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (selected && focused_) {
        RECT focus = rc;
        focus.right -= 1;
        focus.bottom -= 1;
        DrawFocusRect(dc, &focus);
    }

    SelectObject(dc, old_font);
}

void GestureListControl::draw_column_header(HDC dc, const RECT& client) {
    RECT header = { 0, 0, client.right, column_header_height_ };
    HBRUSH brush = CreateSolidBrush(header_background_);
    FillRect(dc, &header, brush);
    DeleteObject(brush);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text_);
    HGDIOBJ old_font = SelectObject(dc, font_);

    const wchar_t* titles[3] = { L"Gesture", L"Command", L"Place" };
    const int indicator_width = scale(16);
    int left = -scroll_x_;
    for (int column = 0; column < 3; ++column) {
        RECT cell = { left + scale(6), 0,
                      left + columns_[column] - scale(4), column_header_height_ };
        if (cell.right > cell.left && cell.right > 0 && cell.left < client.right) {
            const GestureSortColumn sort_column = gesture_sort_column_from_index(column);
            const bool active = sort_state_.column == sort_column;
            const wchar_t* indicator = L"↕";
            if (active) {
                indicator = sort_state_.direction == GestureSortDirection::ascending ? L"▲" : L"▼";
            }

            RECT text_cell = cell;
            text_cell.right -= indicator_width;
            RECT indicator_cell = cell;
            indicator_cell.left = text_cell.right;
            if (text_cell.right > text_cell.left) {
                SetTextColor(dc, text_);
                DrawTextW(dc, titles[column], -1, &text_cell,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            SetTextColor(dc, active ? text_ : muted_);
            DrawTextW(dc, indicator, -1, &indicator_cell,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        left += columns_[column];
    }

    const int brightness = (GetRValue(header_background_) + GetGValue(header_background_) +
                            GetBValue(header_background_)) / 3;
    HPEN pen = CreatePen(PS_SOLID, 1, brightness > 128 ? RGB(180, 180, 180) : RGB(90, 90, 90));
    HGDIOBJ old_pen = SelectObject(dc, pen);

    left = -scroll_x_;
    for (int column = 0; column < 2; ++column) {
        left += columns_[column];
        if (left >= 0 && left < client.right) {
            MoveToEx(dc, left, scale(3), nullptr);
            LineTo(dc, left, column_header_height_ - scale(3));
        }
    }
    MoveToEx(dc, 0, column_header_height_ - 1, nullptr);
    LineTo(dc, client.right, column_header_height_ - 1);

    SelectObject(dc, old_pen);
    DeleteObject(pen);
    SelectObject(dc, old_font);
}

void GestureListControl::paint(HDC target, const RECT& client) {
    if (client.right - client.left <= 0 || client.bottom - client.top <= 0) return;

    // Двойная буферизация: без неё при прокрутке заметно мерцание.
    HDC memory_dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right - client.left, client.bottom - client.top);
    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);

    HBRUSH background = CreateSolidBrush(background_);
    FillRect(memory_dc, &client, background);
    DeleteObject(background);

    SelectObject(memory_dc, font_);

    // Строки не должны залезать на закреплённую шапку при прокрутке.
    const int rows_dc = SaveDC(memory_dc);
    IntersectClipRect(memory_dc, 0, column_header_height_, client.right, client.bottom);

    int y = column_header_height_ - scroll_;
    if (rows_ != nullptr) {
        for (std::size_t i = 0; i < rows_->size(); ++i) {
            const GestureListRow& row = (*rows_)[i];
            const int height = row_height(row.is_header);
            if (y + height > column_header_height_ && y < client.bottom) {
                RECT row_rect = { 0, y, client.right, y + height };
                draw_row(memory_dc, row_rect, row, static_cast<int>(i) == selected_);
            }
            y += height;
        }
    }

    RestoreDC(memory_dc, rows_dc);

    if (rows_ == nullptr || rows_->empty()) {
        RECT empty = client;
        empty.top = column_header_height_;
        empty.left += scale(10);
        SetBkMode(memory_dc, TRANSPARENT);
        SetTextColor(memory_dc, muted_);
        DrawTextW(memory_dc, L"No bindings - click Reset.", -1, &empty,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    draw_column_header(memory_dc, client);

    BitBlt(target, 0, 0, client.right - client.left, client.bottom - client.top, memory_dc, 0, 0, SRCCOPY);

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
}

LRESULT GestureListControl::window_proc(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint_struct = {};
        HDC dc = BeginPaint(hwnd_, &paint_struct);
        RECT client = {};
        GetClientRect(hwnd_, &client);
        if (dc != nullptr) paint(dc, client);
        EndPaint(hwnd_, &paint_struct);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        update_scroll_info();
        invalidate();
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT point = {};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            if (resizing_column_ >= 0 || column_divider_at(point.x, point.y) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            if (column_header_at(point.x, point.y) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_SETFOCUS:
        focused_ = true;
        invalidate();
        return 0;

    case WM_KILLFOCUS:
        focused_ = false;
        invalidate();
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int divider = column_divider_at(x, y);
        if (divider >= 0) {
            columns_customized_ = true;
            resizing_column_ = divider;
            resize_anchor_x_ = x;
            resize_start_width_ = columns_[divider];
            SetCapture(hwnd_);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return 0;
        }

        const int column = column_header_at(x, y);
        if (column >= 0) {
            sort_state_ = next_gesture_sort_state(sort_state_, gesture_sort_column_from_index(column));
            if (on_sort_) on_sort_();
            invalidate();
            return 0;
        }

        if (y >= column_header_height_) {
            SetFocus(hwnd_);
            const int row = row_at(y);
            if (row >= 0) select_row(row, true);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (resizing_column_ >= 0) {
            int width = resize_start_width_ + GET_X_LPARAM(lparam) - resize_anchor_x_;
            const int minimum = min_column_width(resizing_column_);
            if (width < minimum) width = minimum;
            if (columns_[resizing_column_] != width) {
                columns_[resizing_column_] = width;
                update_scroll_info();
                invalidate();
            }
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (resizing_column_ >= 0) {
            resizing_column_ = -1;
            if (GetCapture() == hwnd_) ReleaseCapture();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        resizing_column_ = -1;
        return 0;

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        scroll_to(scroll_ - (delta / WHEEL_DELTA) * row_height_ * 3);
        return 0;
    }

    case WM_VSCROLL: {
        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int page = client.bottom - client.top - column_header_height_;

        switch (LOWORD(wparam)) {
        case SB_LINEUP: scroll_to(scroll_ - row_height_); break;
        case SB_LINEDOWN: scroll_to(scroll_ + row_height_); break;
        case SB_PAGEUP: scroll_to(scroll_ - page); break;
        case SB_PAGEDOWN: scroll_to(scroll_ + page); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO info = {};
            info.cbSize = sizeof(info);
            info.fMask = SIF_TRACKPOS;
            if (GetScrollInfo(hwnd_, SB_VERT, &info)) scroll_to(info.nTrackPos);
            break;
        }
        case SB_TOP: scroll_to(0); break;
        case SB_BOTTOM: scroll_to(content_height()); break;
        default: break;
        }
        return 0;
    }

    case WM_HSCROLL: {
        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int page = client.right - client.left;

        switch (LOWORD(wparam)) {
        case SB_LINELEFT: scroll_horizontal_to(scroll_x_ - scale(16)); break;
        case SB_LINERIGHT: scroll_horizontal_to(scroll_x_ + scale(16)); break;
        case SB_PAGELEFT: scroll_horizontal_to(scroll_x_ - page); break;
        case SB_PAGERIGHT: scroll_horizontal_to(scroll_x_ + page); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO info = {};
            info.cbSize = sizeof(info);
            info.fMask = SIF_TRACKPOS;
            if (GetScrollInfo(hwnd_, SB_HORZ, &info)) scroll_horizontal_to(info.nTrackPos);
            break;
        }
        case SB_LEFT: scroll_horizontal_to(0); break;
        case SB_RIGHT: scroll_horizontal_to(columns_width()); break;
        default: break;
        }
        return 0;
    }

    case WM_KEYDOWN: {
        const auto navigate = [this](GestureRowNavigation navigation) {
            if (rows_ == nullptr) return;
            const int target = gesture_navigation_row(*rows_, selected_, navigation);
            if (target >= 0) select_row(target, true);
        };

        switch (wparam) {
        case VK_UP: navigate(GestureRowNavigation::previous); break;
        case VK_DOWN: navigate(GestureRowNavigation::next); break;
        case VK_PRIOR: navigate(GestureRowNavigation::page_previous); break;
        case VK_NEXT: navigate(GestureRowNavigation::page_next); break;
        case VK_HOME: navigate(GestureRowNavigation::first); break;
        case VK_END: navigate(GestureRowNavigation::last); break;
        default: break;
        }

        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
//  Реализация страницы настроек
// ---------------------------------------------------------------------------

class GesturePreferencesInstance : public preferences_page_instance {
public:
    GesturePreferencesInstance(HWND parent, preferences_page_callback::ptr callback);
    ~GesturePreferencesInstance();

    t_uint32 get_state() override;
    fb2k::hwnd_t get_wnd() override;
    void apply() override;
    void reset() override;

private:
    static LRESULT CALLBACK window_proc_static(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK command_list_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                                       LPARAM lparam, UINT_PTR subclass_id,
                                                       DWORD_PTR ref_data);
    static LRESULT CALLBACK help_button_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                                      LPARAM lparam, UINT_PTR subclass_id,
                                                      DWORD_PTR ref_data);
    LRESULT window_proc(UINT msg, WPARAM wparam, LPARAM lparam);

    static void register_window_class();
    void install_command_list_scroll_handler();

    void create_children();
    void create_tooltips();
    void add_tooltip(HWND control, const wchar_t* text);
    void on_help();
    void hide_tracking_help();
    void set_tooltips_active(bool active);
    //! Подогнать страницу под клиентскую область родителя (хост не всегда
    //! присылает WM_SIZE, из-за чего низ страницы обрезался).
    void fit_to_parent();
    void layout();
    void apply_theme();
    void apply_list_theme();

    void load_from_settings();
    void restore_editor_draft_session(const std::optional<GestureEditorState>& saved);
    void load_new_editor();
    void load_binding_editor(int binding_index);
    void sync_editor_controls();
    void refresh_list();
    void refresh_gesture_text();
    void refresh_command_list();
    void update_buttons();
    void update_command_status();
    void on_editor_changed();
    void sync_page_dirty(bool force_notify = false);

    EditorGuardDecision show_editor_guard(const EditorPendingAction& action);
    void request_editor_action(const EditorPendingAction& action);
    void execute_editor_action(const EditorPendingAction& action);
    bool commit_editor_changes();

    void on_group_changed();
    void on_command_changed();
    void on_place_changed();
    void on_command_choose_another();
    void on_command_retry();
    void on_command_copy_details();
    bool on_list_selection_guard(int binding_index);
    void on_list_selection_changed(int index);
    void on_new();
    bool on_add();
    bool on_update();
    void on_save_editor();
    void on_discard_editor();
    void on_cancel_editor_action();
    void on_delete();
    void on_move(int delta);
    void on_direction(Token token);
    void on_backspace();
    void on_settings_control_changed();

    //! Звенья, введённые в поле "Gesture" (стрелками или текстом).
    std::vector<Token> editor_tokens() const;
    //! Нормализованная запись жеста из поля "Gesture"; пустая строка - ошибка.
    std::string editor_gesture() const;

    void set_dirty(bool dirty);
    int current_group_index() const;
    int current_place_index() const;
    std::string current_group_id() const;
    std::string current_place_id() const;
    int selected_command_index() const;
    GestureBinding current_editor_binding() const;
    //! Индекс привязки в settings_.bindings по отображаемой строке ListView
    //! (при включённом group view порядок строк не совпадает с порядком привязок).
    int binding_index_from_display(int display_row) const;
    int display_row_from_binding_index(int binding_index) const;
    //! Выделить строку, соответствующую привязке.
    void select_binding(int binding_index);
    //! Индекс выбранной привязки (не строки таблицы!).
    int selected_row() const;
    int read_stroke_length() const;

    HWND make_button(const wchar_t* text, int id, DWORD extra_style = 0);
    HWND make_static(const wchar_t* text, DWORD style);
    HWND make_edit(int id, DWORD extra_style = 0);
    HWND make_combo(int id);
    HWND make_checkbox(const wchar_t* text, int id);

    int scale(int value) const { return MulDiv(value, dpi_, 96); }
    //! Ширина текста в пикселях шрифтом страницы (для расчёта ширины кнопок).
    int text_width(const wchar_t* text) const;
    std::wstring command_details_text() const;

    // --- данные ---
    GestureSettings settings_;
    std::vector<CatalogEntry> catalog_;
    GestureEditorState editor_;
    EditorPendingAction pending_action_;
    std::wstring command_status_text_;

    // --- Win32 ---
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;

    //! Строки таблицы: заголовки групп и сами привязки.
    std::vector<GestureListRow> rows_;
    //! Собственный контрол таблицы (см. GestureListControl).
    GestureListControl list_control_;

    HWND count_label_ = nullptr;
    HWND help_button_ = nullptr;
    HWND group_by_category_check_ = nullptr;
    HWND tooltip_ = nullptr;
    HFONT header_font_ = nullptr;

    HWND left_label_ = nullptr;
    HWND right_label_ = nullptr;
    HWND wheel_label_ = nullptr;
    HWND wheel_up_label_ = nullptr;
    HWND wheel_down_label_ = nullptr;
    HWND rocker_label_ = nullptr;
    HWND stroke_label_ = nullptr;
    HWND gesture_label_ = nullptr;
    HWND group_label_ = nullptr;
    HWND command_label_ = nullptr;
    HWND command_choose_button_ = nullptr;
    HWND command_retry_button_ = nullptr;
    HWND command_copy_button_ = nullptr;
    HWND place_label_ = nullptr;

    HWND dir_up_ = nullptr;
    HWND dir_down_ = nullptr;
    HWND dir_left_ = nullptr;
    HWND dir_right_ = nullptr;
    HWND dir_up_left_ = nullptr;
    HWND dir_up_right_ = nullptr;
    HWND dir_down_left_ = nullptr;
    HWND dir_down_right_ = nullptr;
    HWND dir_backspace_ = nullptr;
    HWND wheel_up_ = nullptr;
    HWND wheel_down_ = nullptr;
    HWND rocker_rl_ = nullptr;
    HWND rocker_lr_ = nullptr;

    HWND gesture_edit_ = nullptr;
    HWND group_combo_ = nullptr;
    HWND command_combo_ = nullptr;
    HWND command_list_ = nullptr;
    HWND place_combo_ = nullptr;

    HWND new_button_ = nullptr;
    HWND add_button_ = nullptr;
    HWND update_button_ = nullptr;
    HWND delete_button_ = nullptr;
    HWND move_up_button_ = nullptr;
    HWND move_down_button_ = nullptr;

    HWND stroke_edit_ = nullptr;
    HWND stroke_spin_ = nullptr;
    HWND disable_check_ = nullptr;
    HWND alphabet_check_ = nullptr;
    HWND first_only_check_ = nullptr;

    HFONT font_ = nullptr;
    bool owns_font_ = false;

    HBRUSH background_brush_ = nullptr;
    HBRUSH control_brush_ = nullptr;
    COLORREF background_color_ = RGB(240, 240, 240);
    COLORREF text_color_ = RGB(0, 0, 0);

    int dpi_ = 96;
    bool dirty_ = false;
    bool committed_dirty_ = false;
    bool notified_editor_busy_ = false;
    bool editor_guard_active_ = false;
    bool command_actions_shown_ = false;
    bool updating_ = false;
    bool diagnostics_logged_ = false;
    bool pending_state_notify_ = false;
    bool list_diagnostics_logged_ = false;
    bool help_visible_ = false;
    bool group_by_category_ = true;

    preferences_page_callback::ptr callback_;
    fb2k::CCoreDarkModeHooks dark_mode_;
};

void GesturePreferencesInstance::register_window_class() {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &GesturePreferencesInstance::window_proc_static;
    wc.hInstance = core_api::get_my_instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // фон рисуем сами (нужна поддержка тёмной темы)
    wc.lpszClassName = kPreferencesPageClass;
    RegisterClassExW(&wc);

    registered = true;
}

GesturePreferencesInstance::GesturePreferencesInstance(HWND parent,
                                                       preferences_page_callback::ptr callback)
    : callback_(callback) {
    const auto draft_session = g_editor_draft_session;
    register_window_class();

    settings_ = GestureConfig::instance().load_from_storage();

    RECT parent_rect = {};
    if (parent != nullptr) GetClientRect(parent, &parent_rect);

    const int width = parent_rect.right > 0 ? parent_rect.right : 640;
    const int height = parent_rect.bottom > 0 ? parent_rect.bottom : 520;

    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kPreferencesPageClass, L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP,
                            0, 0, width, height, parent, nullptr, core_api::get_my_instance(), this);
    PFC_ASSERT(hwnd_ != nullptr);
    if (hwnd_ == nullptr) return;

    dpi_ = get_window_dpi(hwnd_);

    create_children();
    apply_theme();

    // Пустая таблица после Apply - осознанная настройка, а не первый запуск.
    // Defaults восстанавливаются явной кнопкой Reset или самим хранилищем при первом запуске.
    load_from_settings();
    restore_editor_draft_session(draft_session);

    layout();

    dark_mode_.AddDialogWithControls(hwnd_);
}

GesturePreferencesInstance::~GesturePreferencesInstance() {
    if (tooltip_ != nullptr) {
        hide_tracking_help();
        if (IsWindow(tooltip_)) DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (background_brush_ != nullptr) DeleteObject(background_brush_);
    if (control_brush_ != nullptr) DeleteObject(control_brush_);
    if (header_font_ != nullptr) DeleteObject(header_font_);
    if (owns_font_ && font_ != nullptr) DeleteObject(font_);
}

// ---------------------------------------------------------------------------
//  preferences_page_instance
// ---------------------------------------------------------------------------

t_uint32 GesturePreferencesInstance::get_state() {
    t_uint32 state = preferences_state::dark_mode_supported | preferences_state::resettable;
    if (dirty_) state |= preferences_state::changed;
    if (editor_.dirty) state |= preferences_state::busy;
    return state;
}

fb2k::hwnd_t GesturePreferencesInstance::get_wnd() {
    return hwnd_;
}

void GesturePreferencesInstance::apply() {
    if (editor_.dirty) {
        request_editor_action({ EditorActionType::apply });
        return;
    }

    settings_.stroke_length = read_stroke_length();
    settings_.disabled = Button_GetCheck(disable_check_) == BST_CHECKED;
    settings_.alphabet = Button_GetCheck(alphabet_check_) == BST_CHECKED;
    settings_.first_only = Button_GetCheck(first_only_check_) == BST_CHECKED;

    GestureConfig::instance().apply(settings_);
    GestureEngine::instance().reload();

    set_dirty(false);
}

void GesturePreferencesInstance::reset() {
    if (editor_.dirty) {
        request_editor_action({ EditorActionType::reset });
        return;
    }

    settings_ = GestureConfig::defaults();
    set_dirty(true);
    load_from_settings();
}

// ---------------------------------------------------------------------------
//  Создание контролов
// ---------------------------------------------------------------------------

HWND GesturePreferencesInstance::make_button(const wchar_t* text, int id, DWORD extra_style) {
    HWND wnd = CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extra_style,
                               0, 0, 40, 24, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               core_api::get_my_instance(), nullptr);
    if (font_ != nullptr) SendMessageW(wnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return wnd;
}

HWND GesturePreferencesInstance::make_static(const wchar_t* text, DWORD style) {
    HWND wnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                               0, 0, 40, 16, hwnd_, nullptr, core_api::get_my_instance(), nullptr);
    if (font_ != nullptr) SendMessageW(wnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return wnd;
}

HWND GesturePreferencesInstance::make_edit(int id, DWORD extra_style) {
    HWND wnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra_style,
                               0, 0, 100, 22, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               core_api::get_my_instance(), nullptr);
    if (font_ != nullptr) SendMessageW(wnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return wnd;
}

HWND GesturePreferencesInstance::make_combo(int id) {
    HWND wnd = CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                               0, 0, 160, 240, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               core_api::get_my_instance(), nullptr);
    if (font_ != nullptr) SendMessageW(wnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return wnd;
}

HWND GesturePreferencesInstance::make_checkbox(const wchar_t* text, int id) {
    HWND wnd = CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               0, 0, 100, 18, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               core_api::get_my_instance(), nullptr);
    if (font_ != nullptr) SendMessageW(wnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return wnd;
}

void GesturePreferencesInstance::create_children() {
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        metrics.lfMessageFont.lfHeight = -MulDiv(9, dpi_, 72);
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (font_ == nullptr) {
        font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        owns_font_ = false;
    } else {
        owns_font_ = true;
    }

    const HINSTANCE instance = core_api::get_my_instance();

    INITCOMMONCONTROLSEX common_controls = {};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES |
                            ICC_WIN95_CLASSES;
    InitCommonControlsEx(&common_controls);

    // Жирный шрифт для заголовков групп внутри таблицы.
    if (font_ != nullptr) {
        LOGFONTW log_font = {};
        if (GetObjectW(font_, sizeof(log_font), &log_font) == sizeof(log_font)) {
            log_font.lfWeight = FW_SEMIBOLD;
            header_font_ = CreateFontIndirectW(&log_font);
        }
    }

    // Таблица привязок - собственный контрол: он рисует и строки, и заголовки
    // групп, поэтому вид не зависит от темы foobar2000.
    list_ = list_control_.create(hwnd_, kIdList);
    if (list_ == nullptr) {
        FB2K_console_formatter() << "Mouse Gesture Host: failed to create the gesture list ("
                                 << static_cast<unsigned long>(GetLastError()) << ")";
    } else {
        list_control_.set_fonts(font_, header_font_, dpi_);
        list_control_.set_owner_service(this);
        list_control_.set_select_guard_handler([this](int binding_index) {
            return on_list_selection_guard(binding_index);
        });
        list_control_.set_select_handler([this](int binding_index) {
            on_list_selection_changed(binding_index);
            update_buttons();
        });
        list_control_.set_sort_handler([this] {
            refresh_list();
        });
    }

    count_label_ = make_static(L"", SS_LEFT | SS_CENTERIMAGE);
    group_by_category_check_ = make_checkbox(L"Group by category", kIdGroupByCategory);
    Button_SetCheck(group_by_category_check_, group_by_category_ ? BST_CHECKED : BST_UNCHECKED);
    // Текст окна остаётся доступным именем Help; видимый маркер рисуется как "?".
    help_button_ = make_button(L"Help", kIdHelp, BS_OWNERDRAW);
    if (help_button_ != nullptr) {
        SetWindowSubclass(help_button_, help_button_subclass_proc, kHelpButtonSubclassId,
                          reinterpret_cast<DWORD_PTR>(this));
    }

    left_label_ = make_static(L"left", SS_LEFT);
    right_label_ = make_static(L"right", SS_LEFT);
    wheel_label_ = make_static(L"wheel", SS_LEFT);
    wheel_up_label_ = make_static(L"up", SS_CENTER);
    wheel_down_label_ = make_static(L"down", SS_CENTER);
    rocker_label_ = make_static(L"rocker", SS_LEFT);
    stroke_label_ = make_static(L"Stroke Length", SS_LEFT);
    gesture_label_ = make_static(L"Gesture", SS_LEFT);
    group_label_ = make_static(L"Group", SS_LEFT);
    command_label_ = make_static(L"Command", SS_LEFT);
    place_label_ = make_static(L"Place", SS_LEFT);

    dir_up_ = make_button(arrow("\xE2\x86\x91").c_str(), kIdDirUp);
    dir_down_ = make_button(arrow("\xE2\x86\x93").c_str(), kIdDirDown);
    dir_left_ = make_button(arrow("\xE2\x86\x90").c_str(), kIdDirLeft);
    dir_right_ = make_button(arrow("\xE2\x86\x92").c_str(), kIdDirRight);
    dir_up_left_ = make_button(arrow("\xE2\x86\x96").c_str(), kIdDirUpLeft);
    dir_up_right_ = make_button(arrow("\xE2\x86\x97").c_str(), kIdDirUpRight);
    dir_down_left_ = make_button(arrow("\xE2\x86\x99").c_str(), kIdDirDownLeft);
    dir_down_right_ = make_button(arrow("\xE2\x86\x98").c_str(), kIdDirDownRight);
    dir_backspace_ = make_button(L"BS", kIdDirBackspace);
    wheel_up_ = make_button(arrow("\xE2\x86\x91").c_str(), kIdWheelUp);
    wheel_down_ = make_button(arrow("\xE2\x86\x93").c_str(), kIdWheelDown);
    rocker_rl_ = make_button(L"R+L", kIdRockerRightLeft);
    rocker_lr_ = make_button(L"L+R", kIdRockerLeftRight);

    gesture_edit_ = make_edit(kIdGestureEdit, ES_READONLY);
    group_combo_ = make_combo(kIdGroupCombo);
    command_combo_ = make_combo(kIdCommandCombo);
    place_combo_ = make_combo(kIdPlaceCombo);
    install_command_list_scroll_handler();

    new_button_ = make_button(L"New", kIdNew);
    add_button_ = make_button(L"Add New", kIdAdd);
    update_button_ = make_button(L"Update", kIdUpdate);
    delete_button_ = make_button(L"Delete", kIdDelete);
    move_up_button_ = make_button(L"Up", kIdMoveUp);
    move_down_button_ = make_button(L"Down", kIdMoveDown);

    command_choose_button_ = make_button(L"Выбрать другую", kIdCommandChooseAnother);
    command_retry_button_ = make_button(L"Повторить поиск", kIdCommandRetry);
    command_copy_button_ = make_button(L"Скопировать детали", kIdCommandCopyDetails);
    ShowWindow(command_choose_button_, SW_HIDE);
    ShowWindow(command_retry_button_, SW_HIDE);
    ShowWindow(command_copy_button_, SW_HIDE);

    stroke_edit_ = make_edit(kIdStrokeEdit, ES_NUMBER | ES_RIGHT);
    stroke_spin_ = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                                   WS_CHILD | WS_VISIBLE | UDS_ARROWKEYS | UDS_SETBUDDYINT | UDS_NOTHOUSANDS,
                                   0, 0, scale(16), scale(24), hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStrokeSpin)),
                                   instance, nullptr);
    if (font_ != nullptr) SendMessageW(stroke_spin_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(stroke_spin_, UDM_SETBUDDY, reinterpret_cast<WPARAM>(stroke_edit_), 0);
    SendMessageW(stroke_spin_, UDM_SETRANGE32, static_cast<WPARAM>(kMinStrokeLength),
                 static_cast<LPARAM>(kMaxStrokeLength));

    disable_check_ = make_checkbox(L"Disable mouse gesture", kIdDisable);
    alphabet_check_ = make_checkbox(L"Represent direction by the alphabet", kIdAlphabet);
    first_only_check_ = make_checkbox(L"Execute only the first command", kIdFirstOnly);

    for (const auto& entry : command_groups()) {
        const std::wstring label = wide_from_utf8(entry.second);
        ComboBox_AddString(group_combo_, label.c_str());
    }
    ComboBox_SetCurSel(group_combo_, 0);

    for (const auto& entry : command_places()) {
        const std::wstring label = wide_from_utf8(entry.second);
        ComboBox_AddString(place_combo_, label.c_str());
    }
    ComboBox_SetCurSel(place_combo_, 0);

    create_tooltips();
}

// ---------------------------------------------------------------------------
//  Контекстные подсказки
// ---------------------------------------------------------------------------

void GesturePreferencesInstance::add_tooltip(HWND control, const wchar_t* text) {
    if (tooltip_ == nullptr || control == nullptr || text == nullptr) return;

    TOOLINFOW tool = {};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    tool.hwnd = hwnd_;
    tool.uId = reinterpret_cast<UINT_PTR>(control);
    tool.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

void GesturePreferencesInstance::create_tooltips() {
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               hwnd_, nullptr, core_api::get_my_instance(), nullptr);
    if (tooltip_ == nullptr) {
        FB2K_console_formatter() << "Mouse Gesture Host: failed to create preferences tooltips ("
                                 << static_cast<unsigned long>(GetLastError()) << ")";
        return;
    }

    SendMessageW(tooltip_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, scale(360));

    add_tooltip(dir_up_left_, L"Up-left: add UL to Gesture.");
    add_tooltip(dir_up_, L"Up: add U to Gesture.");
    add_tooltip(dir_up_right_, L"Up-right: add UR to Gesture.");
    add_tooltip(dir_left_, L"Left: add L to Gesture.");
    add_tooltip(dir_backspace_, L"Backspace: remove the last token from Gesture.");
    add_tooltip(dir_right_, L"Right: add R to Gesture.");
    add_tooltip(dir_down_left_, L"Down-left: add DL to Gesture.");
    add_tooltip(dir_down_, L"Down: add D to Gesture.");
    add_tooltip(dir_down_right_, L"Down-right: add DR to Gesture.");

    add_tooltip(wheel_up_, L"Wheel Up: hold RMB and scroll up; adds WU.");
    add_tooltip(wheel_down_, L"Wheel Down: hold RMB and scroll down; adds WD.");
    add_tooltip(rocker_rl_, L"R+L: hold RMB and click LMB.");
    add_tooltip(rocker_lr_, L"L+R: hold LMB and click RMB.");

    add_tooltip(stroke_label_, kStrokeHelp);
    add_tooltip(stroke_edit_, kStrokeHelp);
    add_tooltip(stroke_spin_, kStrokeHelp);

    add_tooltip(gesture_edit_, L"Gesture: canonical tokens for the binding being edited.");
    add_tooltip(group_combo_, L"Group: choose a main-menu or context-menu command source.");
    add_tooltip(command_combo_, L"Command: choose the command bound to the gesture.");
    add_tooltip(command_choose_button_, L"Выбрать другую: focus the Command list without changing the binding automatically.");
    add_tooltip(command_retry_button_, L"Повторить поиск: rebuild the catalog and resolve only the existing command identity.");
    add_tooltip(command_copy_button_, L"Скопировать детали: copy the full path, reason, and command identity to the clipboard.");
    add_tooltip(place_combo_, L"Place: choose the execution context for the command.");

    add_tooltip(new_button_, L"New: clear the editor after resolving any unsaved changes.");
    add_tooltip(add_button_, L"Add New: stage a new binding on this page; Apply saves the page settings.");
    add_tooltip(update_button_, L"Update: stage changes to the selected binding; Apply saves the page settings.");
    add_tooltip(delete_button_, L"Delete: remove the selected binding.");
    add_tooltip(move_up_button_, L"Up: move earlier in flat Manual order; disabled in sorted or grouped views.");
    add_tooltip(move_down_button_, L"Down: move later in flat Manual order; disabled in sorted or grouped views.");
    add_tooltip(group_by_category_check_,
                L"Group by category: show one header per category; disable for a flat list with category chips. This view option does not change execution order.");
    add_tooltip(help_button_, kPreferencesHelp);

    // Отдельный tracking tool позволяет открыть ту же справку кнопкой Help
    // с клавиатуры, не создавая модальное окно внутри preferences instance.
    TOOLINFOW help_tool = {};
    help_tool.cbSize = sizeof(help_tool);
    // Координаты дополнительно ограничиваются клиентской областью страницы.
    help_tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    help_tool.hwnd = hwnd_;
    help_tool.uId = kHelpTrackingToolId;
    help_tool.lpszText = const_cast<wchar_t*>(kPreferencesHelp);
    GetClientRect(hwnd_, &help_tool.rect);
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&help_tool));
}

void GesturePreferencesInstance::hide_tracking_help() {
    if (tooltip_ != nullptr && IsWindow(tooltip_)) {
        if (help_visible_) {
            TOOLINFOW help_tool = {};
            help_tool.cbSize = sizeof(help_tool);
            help_tool.hwnd = hwnd_;
            help_tool.uId = kHelpTrackingToolId;
            SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&help_tool));
        }
        // Заодно закрываем обычный hover tooltip при Esc или клике вне.
        SendMessageW(tooltip_, TTM_POP, 0, 0);
    }
    help_visible_ = false;
}

void GesturePreferencesInstance::on_help() {
    if (tooltip_ == nullptr || help_button_ == nullptr || help_visible_) return;

    SendMessageW(tooltip_, TTM_POP, 0, 0);
    RECT client_rect = {};
    GetClientRect(hwnd_, &client_rect);
    int tooltip_width = client_rect.right - client_rect.left - 2 * scale(4);
    if (tooltip_width > scale(360)) tooltip_width = scale(360);
    if (tooltip_width > 0) SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, tooltip_width);

    RECT button_rect = {};
    GetWindowRect(help_button_, &button_rect);
    const int gap = scale(4);
    int x = button_rect.left;
    int y = button_rect.bottom + gap;
    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));

    TOOLINFOW help_tool = {};
    help_tool.cbSize = sizeof(help_tool);
    help_tool.hwnd = hwnd_;
    help_tool.uId = kHelpTrackingToolId;
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&help_tool));
    help_visible_ = true;

    // После показа размер известен: сперва пытаемся оставить справку под
    // маркером, затем переносим её выше и ограничиваем клиентской областью.
    RECT tooltip_rect = {};
    if (GetWindowRect(tooltip_, &tooltip_rect)) {
        const int width = tooltip_rect.right - tooltip_rect.left;
        const int height = tooltip_rect.bottom - tooltip_rect.top;
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&client_rect), 2);
        if (x + width > client_rect.right) x = client_rect.right - width;
        if (y + height > client_rect.bottom) y = button_rect.top - height - gap;
        if (x < client_rect.left) x = client_rect.left;
        if (y < client_rect.top) y = client_rect.top;
        SendMessageW(tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    }
}

void GesturePreferencesInstance::set_tooltips_active(bool active) {
    if (tooltip_ == nullptr || !IsWindow(tooltip_)) return;
    if (!active) hide_tracking_help();
    SendMessageW(tooltip_, TTM_ACTIVATE, active ? TRUE : FALSE, 0);
}

// ---------------------------------------------------------------------------
//  Разметка
// ---------------------------------------------------------------------------

void GesturePreferencesInstance::fit_to_parent() {
    if (hwnd_ == nullptr) return;

    HWND parent = GetParent(hwnd_);
    if (parent == nullptr) return;

    RECT parent_client = {};
    GetClientRect(parent, &parent_client);
    const int available_width = parent_client.right - parent_client.left;
    const int available_height = parent_client.bottom - parent_client.top;
    if (available_width <= 0 || available_height <= 0) return;

    RECT current = {};
    GetClientRect(hwnd_, &current);
    const int current_width = current.right - current.left;
    const int current_height = current.bottom - current.top;

    if (current_width == available_width && current_height == available_height) return;

    SetWindowPos(hwnd_, nullptr, 0, 0, available_width, available_height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

int GesturePreferencesInstance::text_width(const wchar_t* text) const {
    HDC dc = GetDC(hwnd_);
    if (dc == nullptr) return 0;

    HGDIOBJ old_font = SelectObject(dc, font_);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    SelectObject(dc, old_font);
    ReleaseDC(hwnd_, dc);

    return static_cast<int>(size.cx);
}

void GesturePreferencesInstance::layout() {
    if (hwnd_ == nullptr || list_ == nullptr) return;
    hide_tracking_help();

    RECT client = {};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    if (tooltip_ != nullptr) SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, scale(360));

    // --- общие метрики (в единицах 96 DPI, масштабируются через scale()) ---
    const int margin = scale(10);
    const int gap = scale(6);
    const int label_height = scale(14);
    const int control_height = scale(22);

    // Клиентская область страницы уже учитывает inset хоста; собственную
    // рамку таблицы ставим непосредственно на её верхнюю кромку.
    const int list_top = 0;

    // --- крестовина (D-Pad) -------------------------------------------------
    // Квадратные цели 38 DIP образуют непрерывную сетку 3×3 с BS в центре.
    const int cell = scale(38);

    // Ширины подписей left/right берём по фактическому тексту: фиксированные
    // 44 px сдвигали крестовину вправо и слово "left" не попадало на одну
    // вертикаль с "wheel"/"rocker".
    int side_left = text_width(L"left") + scale(2);
    int side_right = text_width(L"right") + scale(2);
    if (side_left < scale(20)) side_left = scale(20);
    if (side_right < scale(24)) side_right = scale(24);
    const int label_gap = scale(6);

    const int pad_height = 3 * cell;

    // --- wheel / rocker / stroke / галочки ----------------------------------
    const int wheel_height = control_height + label_height;          // кнопки + подписи up/down
    const int rocker_height = control_height;

    // Вторая и третья галочки делят строку, если им действительно хватает
    // ширины для текста. Это освобождает ещё один ряд для таблицы.
    const int check_height = scale(18);
    const int check_gap = scale(4);
    const int check_row_step = check_height + check_gap;
    const int alphabet_check_width = text_width(L"Represent direction by the alphabet") + scale(30);
    const int first_only_check_width = text_width(L"Execute only the first command") + scale(30);
    const int widest_check = alphabet_check_width > first_only_check_width
        ? alphabet_check_width : first_only_check_width;
    const bool two_columns = 2 * widest_check + gap <= width - 2 * margin;
    const int checks_rows = two_columns ? 2 : 3;
    const int checks_height = check_height * checks_rows + check_gap * (checks_rows - 1);

    // Компактные, но одинаковые вертикальные интервалы между нижними группами.
    const int pad_to_wheel_gap = scale(6);
    const int wheel_to_rocker_gap = scale(4);
    const int rocker_to_stroke_gap = scale(6);
    const int stroke_to_checks_gap = scale(6);

    // Левая колонка: крестовина + wheel + rocker + stroke + галочки.
    const int left_content_height = pad_height + pad_to_wheel_gap + wheel_height + wheel_to_rocker_gap +
                                    rocker_height + rocker_to_stroke_gap + control_height +
                                    stroke_to_checks_gap + checks_height;

    // --- левая колонка: ровно по ширине крестовины --------------------------
    // Подпись "left" начинается ровно на margin - на одной вертикали с
    // "wheel", "rocker" и "Stroke Length". Крестовина идёт сразу за ней,
    // поэтому вся левая колонка получается минимально возможной, а форма -
    // максимально широкой.
    const int pad_x = margin + side_left + label_gap;

    // Правый край крестовины (с подписью "right") задаёт ширину левой колонки.
    const int pad_right = pad_x + 3 * cell + label_gap + side_right;
    const int left_width = pad_right - margin;

    // Остальное отдаём форме: поля Gesture/Group/Command/Place получаются
    // максимально широкими, как в эталоне.
    const int form_x_early = margin + left_width + scale(14);
    const int form_width_early = width - margin - form_x_early;

    // --- ширины кнопок по фактической ширине текста -------------------------
    struct ButtonSpec {
        HWND hwnd;
        const wchar_t* text;
        int min_width;
    };

    constexpr int kButtonCount = 6;
    const ButtonSpec button_specs[kButtonCount] = {
        { new_button_, L"New", 60 },
        { add_button_, L"Add New", 96 },
        { update_button_, L"Update", 72 },
        { delete_button_, L"Delete", 72 },
        { move_up_button_, L"Up", 60 },
        { move_down_button_, L"Down", 60 },
    };

    const int button_padding = scale(14);
    int button_widths[kButtonCount] = {};
    for (int i = 0; i < kButtonCount; ++i) {
        const int needed = text_width(button_specs[i].text) + 2 * button_padding;
        button_widths[i] = needed > scale(button_specs[i].min_width)
            ? needed
            : scale(button_specs[i].min_width);
    }

    // Раскладываем кнопки по рядам жадно: в ряд попадает столько кнопок,
    // сколько реально помещается в ширину формы. Так раскладка работает
    // при любом DPI и любой ширине страницы, а не только в 1-2 ряда.
    int button_row_of[kButtonCount] = {};
    int button_rows = 1;
    {
        int used = 0;
        for (int i = 0; i < kButtonCount; ++i) {
            if (button_widths[i] > form_width_early) {
                button_widths[i] = form_width_early;
            }

            if (used > 0 && used + button_widths[i] > form_width_early) {
                ++button_rows;
                used = 0;
            }

            button_row_of[i] = button_rows - 1;
            used += button_widths[i] + gap;
        }
    }

    const ButtonSpec command_action_specs[3] = {
        { command_choose_button_, L"Выбрать другую", 110 },
        { command_retry_button_, L"Повторить поиск", 110 },
        { command_copy_button_, L"Скопировать детали", 120 },
    };
    int command_action_widths[3] = {};
    int command_action_row_of[3] = {};
    int command_action_rows = command_actions_shown_ ? 1 : 0;
    int command_action_used = 0;
    for (int i = 0; i < 3; ++i) {
        const int needed = text_width(command_action_specs[i].text) + 2 * button_padding;
        command_action_widths[i] = needed > scale(command_action_specs[i].min_width)
            ? needed
            : scale(command_action_specs[i].min_width);
        if (command_action_widths[i] > form_width_early) command_action_widths[i] = form_width_early;
        if (!command_actions_shown_) continue;
        if (command_action_used > 0 && command_action_used + command_action_widths[i] > form_width_early) {
            ++command_action_rows;
            command_action_used = 0;
        }
        command_action_row_of[i] = command_action_rows - 1;
        command_action_used += command_action_widths[i] + gap;
    }
    const int command_actions_height = command_action_rows > 0
        ? command_action_rows * control_height + (command_action_rows - 1) * gap + scale(4)
        : 0;

    // --- правая колонка: 4 строки формы + действия с командой + кнопки -----
    const int row_step = control_height + scale(6);
    const int right_content_height = 4 * row_step + command_actions_height + scale(8) +
                                     button_rows * control_height + (button_rows - 1) * gap + scale(6);

    // Возвращаем таблице фактически освобождённую высоту. Если нижнюю область
    // ограничивает левая колонка, её контролы не сжимаются ради формального +hint.
    const int lower_height = left_content_height > right_content_height ? left_content_height
                                                                        : right_content_height;

    // --- подпись "Всего привязок" над крестовиной ---------------------------
    const int count_height = scale(16);
    const int count_gap = scale(4);

    const int count_top = height - margin - lower_height - count_gap - count_height;
    int list_height = count_top - scale(6) - list_top;

    const int min_list_height = scale(80);
    if (list_height < min_list_height) list_height = min_list_height;

    const int list_width = width - 2 * margin;
    MoveWindow(list_, margin, list_top, list_width, list_height, TRUE);

    // Ширины колонок и обе полосы прокрутки пересчитываются по фактической
    // клиентской области таблицы (без рамки и вертикального скроллбара).
    list_control_.refresh();

    // Счётчик, режим группировки и Help делят существующую строку, поэтому
    // дополнительные элементы не отнимают высоту у таблицы.
    const int help_width = scale(20);
    int group_width = text_width(L"Group by category") + scale(24);
    if (group_width < scale(110)) group_width = scale(110);
    const int help_x = width - margin - help_width;
    const int group_x = help_x - gap - group_width;
    const int count_x = margin + scale(6);
    int count_width = group_x - gap - count_x;
    if (count_width < 0) count_width = 0;
    MoveWindow(count_label_, count_x, count_top, count_width, count_height, TRUE);
    MoveWindow(group_by_category_check_, group_x, count_top, group_width, count_height, TRUE);
    MoveWindow(help_button_, help_x, count_top, help_width, count_height, TRUE);

    const int lower_top = count_top + count_height + count_gap;

    // --- левая колонка ------------------------------------------------------
    const int pad_top = lower_top;
    const int middle_x = pad_x + cell;
    const int right_x = pad_x + 2 * cell;

    MoveWindow(dir_up_left_, pad_x, pad_top, cell, cell, TRUE);
    MoveWindow(dir_up_, middle_x, pad_top, cell, cell, TRUE);
    MoveWindow(dir_up_right_, right_x, pad_top, cell, cell, TRUE);

    const int middle_y = pad_top + cell;
    const int side_label_y = middle_y + (cell - control_height) / 2;
    MoveWindow(left_label_, margin, side_label_y, side_left, control_height, TRUE);
    MoveWindow(dir_left_, pad_x, middle_y, cell, cell, TRUE);
    MoveWindow(dir_backspace_, middle_x, middle_y, cell, cell, TRUE);
    MoveWindow(dir_right_, right_x, middle_y, cell, cell, TRUE);
    MoveWindow(right_label_, right_x + cell + label_gap, side_label_y, side_right, control_height, TRUE);

    const int bottom_y = pad_top + 2 * cell;
    MoveWindow(dir_down_left_, pad_x, bottom_y, cell, cell, TRUE);
    MoveWindow(dir_down_, middle_x, bottom_y, cell, cell, TRUE);
    MoveWindow(dir_down_right_, right_x, bottom_y, cell, cell, TRUE);

    int y = pad_top + pad_height + pad_to_wheel_gap;

    // --- wheel: подпись слева, кнопки в один ряд, подписи up/down под ними --
    MoveWindow(wheel_label_, margin, y, scale(52), control_height, TRUE);
    const int wheel_x = margin + scale(56);
    MoveWindow(wheel_up_, wheel_x, y, scale(28), control_height, TRUE);
    MoveWindow(wheel_down_, wheel_x + scale(32), y, scale(28), control_height, TRUE);
    MoveWindow(wheel_up_label_, wheel_x, y + control_height, scale(28), label_height, TRUE);
    MoveWindow(wheel_down_label_, wheel_x + scale(32), y + control_height, scale(28), label_height, TRUE);
    y += wheel_height + wheel_to_rocker_gap;

    // --- rocker: подпись слева, кнопки в один ряд ---------------------------
    MoveWindow(rocker_label_, margin, y, scale(52), control_height, TRUE);
    const int rocker_x = margin + scale(56);
    MoveWindow(rocker_rl_, rocker_x, y, scale(48), control_height, TRUE);
    MoveWindow(rocker_lr_, rocker_x + scale(52), y, scale(48), control_height, TRUE);
    y += rocker_height + rocker_to_stroke_gap;

    // --- длина штриха -------------------------------------------------------
    MoveWindow(stroke_label_, margin, y, scale(96), control_height, TRUE);
    MoveWindow(stroke_edit_, margin + scale(100), y, scale(56), control_height - scale(2), TRUE);
    MoveWindow(stroke_spin_, margin + scale(157), y, scale(16), control_height - scale(2), TRUE);
    y += control_height + stroke_to_checks_gap;

    // --- галочки ------------------------------------------------------------
    const int checks_width = width - 2 * margin;

    MoveWindow(disable_check_, margin, y, scale(280), check_height, TRUE);

    if (two_columns) {
        const int check_column_width = (checks_width - gap) / 2;
        MoveWindow(alphabet_check_, margin, y + check_row_step,
                   check_column_width, check_height, TRUE);
        MoveWindow(first_only_check_, margin + check_column_width + gap, y + check_row_step,
                   check_column_width, check_height, TRUE);
        y += check_row_step + check_height;
    } else {
        MoveWindow(alphabet_check_, margin, y + check_row_step, checks_width, check_height, TRUE);
        MoveWindow(first_only_check_, margin, y + 2 * check_row_step, checks_width, check_height, TRUE);
        y += 2 * check_row_step + check_height;
    }

    // --- правая колонка: редактор привязки ---------------------------------
    const int form_x = form_x_early;
    const int form_width = form_width_early;

    // Подписи полей - по фактической ширине самого длинного текста
    // ("Command"), а не по фиксированным 84 px: так поля шире.
    int label_width = text_width(L"Command");
    const int label_gesture = text_width(L"Gesture");
    const int label_group = text_width(L"Group");
    const int label_place = text_width(L"Place");
    if (label_gesture > label_width) label_width = label_gesture;
    if (label_group > label_width) label_width = label_group;
    if (label_place > label_width) label_width = label_place;
    label_width += scale(10);

    int editor_width = form_width - label_width;
    if (editor_width < scale(120)) {
        label_width = scale(60);
        editor_width = form_width - label_width;
    }
    if (editor_width < scale(80)) editor_width = scale(80);

    const int editor_x = form_x + label_width;

    MoveWindow(gesture_label_, form_x, lower_top + scale(4), label_width, label_height, TRUE);
    MoveWindow(gesture_edit_, editor_x, lower_top, editor_width, control_height, TRUE);

    MoveWindow(group_label_, form_x, lower_top + row_step + scale(4), label_width, label_height, TRUE);
    SetWindowPos(group_combo_, nullptr, editor_x, lower_top + row_step, editor_width, scale(240),
                 SWP_NOZORDER | SWP_NOACTIVATE);

    MoveWindow(command_label_, form_x, lower_top + 2 * row_step + scale(4), label_width, label_height, TRUE);
    SetWindowPos(command_combo_, nullptr, editor_x, lower_top + 2 * row_step, editor_width, scale(240),
                 SWP_NOZORDER | SWP_NOACTIVATE);

    int place_y = lower_top + 3 * row_step;
    if (command_actions_shown_) {
        int action_row_x[3] = {};
        for (int i = 0; i < command_action_rows; ++i) action_row_x[i] = form_x;
        for (int i = 0; i < 3; ++i) {
            const int row = command_action_row_of[i];
            const int y = place_y + row * (control_height + gap);
            MoveWindow(command_action_specs[i].hwnd, action_row_x[row], y,
                       command_action_widths[i], control_height, TRUE);
            action_row_x[row] += command_action_widths[i] + gap;
        }
        place_y += command_actions_height;
    }
    MoveWindow(place_label_, form_x, place_y + scale(4), label_width, label_height, TRUE);
    SetWindowPos(place_combo_, nullptr, editor_x, place_y, editor_width, scale(160),
                 SWP_NOZORDER | SWP_NOACTIVATE);

    const int buttons_y = place_y + row_step + scale(8);

    // Кнопки управления таблицей. Ширина каждой уже посчитана по тексту
    // (button_widths выше), ряды - жадно по доступной ширине (button_row_of).
    int row_x[kButtonCount] = {};
    for (int i = 0; i < button_rows; ++i) row_x[i] = form_x;

    for (int i = 0; i < kButtonCount; ++i) {
        const int row = button_row_of[i];
        const int y = buttons_y + row * (control_height + gap);
        MoveWindow(button_specs[i].hwnd, row_x[row], y, button_widths[i], control_height, TRUE);
        row_x[row] += button_widths[i] + gap;
    }
}

// ---------------------------------------------------------------------------
//  Тема
// ---------------------------------------------------------------------------

void GesturePreferencesInstance::apply_theme() {
    const bool dark = dark_mode_ ? true : false;

    background_color_ = dark ? RGB(32, 32, 32) : GetSysColor(COLOR_BTNFACE);
    text_color_ = dark ? RGB(235, 235, 235) : GetSysColor(COLOR_BTNTEXT);

    if (background_brush_ != nullptr) DeleteObject(background_brush_);
    background_brush_ = CreateSolidBrush(background_color_);

    if (control_brush_ != nullptr) DeleteObject(control_brush_);
    control_brush_ = CreateSolidBrush(dark ? RGB(45, 45, 45) : GetSysColor(COLOR_WINDOW));

    if (tooltip_ != nullptr) {
        const COLORREF tooltip_background = dark ? RGB(45, 45, 45) : GetSysColor(COLOR_INFOBK);
        const COLORREF tooltip_text = dark ? RGB(235, 235, 235) : GetSysColor(COLOR_INFOTEXT);
        SendMessageW(tooltip_, TTM_SETTIPBKCOLOR, tooltip_background, 0);
        SendMessageW(tooltip_, TTM_SETTIPTEXTCOLOR, tooltip_text, 0);
    }

    apply_list_theme();

    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, TRUE);
        EnumChildWindows(hwnd_, &invalidate_child_proc, 0);
    }
}

void GesturePreferencesInstance::apply_list_theme() {
    if (list_ == nullptr) return;

    const bool dark = dark_mode_ ? true : false;
    const COLORREF list_background = dark ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW);

    // Полоса прокрутки находится в неклиентской области, поэтому кисти
    // WM_CTLCOLOR* на неё не действуют. Явно назначаем тёмную Explorer-тему
    // самому окну, как того требует реализация coreDarkMode для scrollbar.
    SetWindowTheme(list_, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    RedrawWindow(list_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);

    // Таблица - собственный контрол: он сам рисует строки, заголовки групп
    // и выделение, поэтому просто передаём ему палитру текущей темы.
    list_control_.set_colors(list_background,
                             dark ? RGB(230, 230, 230) : GetSysColor(COLOR_WINDOWTEXT),
                             dark ? RGB(150, 150, 150) : GetSysColor(COLOR_GRAYTEXT),
                             dark ? RGB(0, 90, 158) : GetSysColor(COLOR_HIGHLIGHT),
                             dark ? RGB(255, 255, 255) : GetSysColor(COLOR_HIGHLIGHTTEXT),
                             dark ? RGB(45, 45, 45) : GetSysColor(COLOR_BTNFACE));
}

// ---------------------------------------------------------------------------
//  Данные <-> UI
// ---------------------------------------------------------------------------

void GesturePreferencesInstance::load_from_settings() {
    updating_ = true;

    const std::wstring stroke = std::to_wstring(settings_.stroke_length);
    SetWindowTextW(stroke_edit_, stroke.c_str());
    Button_SetCheck(disable_check_, settings_.disabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(alphabet_check_, settings_.alphabet ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(first_only_check_, settings_.first_only ? BST_CHECKED : BST_UNCHECKED);

    refresh_list();
    load_new_editor();

    updating_ = false;
    update_buttons();
}

void GesturePreferencesInstance::restore_editor_draft_session(
    const std::optional<GestureEditorState>& saved) {
    if (!saved.has_value() || !saved->dirty) return;

    GestureEditorState restored = restore_editor_draft(saved.value(), settings_.bindings);
    if (restored.mode == GestureEditorMode::idle) return;

    pending_action_ = {};
    editor_ = std::move(restored);
    if (editor_.mode == GestureEditorMode::edit) {
        select_binding(editor_.binding_index);
    } else {
        list_control_.clear_selection();
    }
    sync_editor_controls();
    sync_page_dirty(true);
}

void GesturePreferencesInstance::load_new_editor() {
    pending_action_ = {};
    list_control_.clear_selection();
    editor_ = make_new_editor_state();
    sync_editor_controls();
    sync_page_dirty();
}

void GesturePreferencesInstance::load_binding_editor(int binding_index) {
    if (binding_index < 0 || binding_index >= static_cast<int>(settings_.bindings.size())) return;
    pending_action_ = {};
    editor_ = make_edit_editor_state(settings_.bindings[static_cast<std::size_t>(binding_index)], binding_index);
    sync_editor_controls();
    sync_page_dirty();
}

void GesturePreferencesInstance::sync_editor_controls() {
    const bool was_updating = updating_;
    updating_ = true;

    refresh_gesture_text();

    int group_index = 0;
    const auto& groups = command_groups();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (editor_.draft.group == groups[i].first) group_index = static_cast<int>(i);
    }
    ComboBox_SetCurSel(group_combo_, group_index);
    refresh_command_list();

    int command_index = -1;
    if (!editor_.draft.command_path.empty()) {
        const CommandMatch match = resolve_command(editor_.draft, catalog_);
        command_index = match.status == CommandResolution::found
            ? static_cast<int>(match.index)
            : -1;
        if (command_index < 0) {
            // Сохранённую команду показываем, но не делаем вид, что она исполняема.
            CatalogEntry missing;
            missing.path = editor_.draft.command_path;
            missing.group = editor_.draft.group;
            missing.command_guid = editor_.draft.command_guid;
            missing.subcommand_guid = editor_.draft.subcommand_guid;
            missing.node_path = editor_.draft.node_path;
            missing.selectable = false;
            catalog_.push_back(missing);

            const char* status = match.status == CommandResolution::ambiguous
                ? "conflict" : "unavailable";
            const std::wstring label = wide_from_utf8(missing.path + " [" + status + "]");
            ComboBox_AddString(command_combo_, label.c_str());
            command_index = static_cast<int>(catalog_.size()) - 1;

            FB2K_console_formatter() << "Mouse Gesture Host: binding \"" << editor_.draft.gesture.c_str()
                                     << "\" -> \"" << editor_.draft.command_path.c_str() << "\" is " << status
                                     << ": " << match.message.c_str();
        }
    }
    ComboBox_SetCurSel(command_combo_, command_index < 0 ? -1 : command_index + 1);

    int place_index = 0;
    const auto& places = command_places();
    for (std::size_t i = 0; i < places.size(); ++i) {
        if (editor_.draft.place == places[i].first) place_index = static_cast<int>(i);
    }
    ComboBox_SetCurSel(place_combo_, place_index);

    updating_ = was_updating;
    update_buttons();
}

void GesturePreferencesInstance::refresh_list() {
    if (list_ == nullptr) return;

    const bool was_updating = updating_;
    updating_ = true;

    const int previous = selected_row();
    const GestureSortState sort = list_control_.sort_state();

    // Сначала всегда строим плоский manual view. Сортировка получает только его
    // копию и физически не может изменить порядок исполнения в settings_.bindings.
    std::vector<GestureListRow> manual_rows;
    manual_rows.reserve(settings_.bindings.size());
    for (std::size_t i = 0; i < settings_.bindings.size(); ++i) {
        const GestureBinding& binding = settings_.bindings[i];
        GestureListRow row;
        row.binding_index = static_cast<int>(i);
        row.gesture = wide_from_utf8(format_gesture(binding.gesture, settings_.alphabet));
        row.command = wide_from_utf8(std::string(group_label(binding.group)) + ": " + binding.command_path);
        row.place = wide_from_utf8(place_label(binding.place));
        prepare_gesture_list_row(row, binding);
        manual_rows.push_back(std::move(row));
    }

    rows_ = make_gesture_list_view(manual_rows, sort, group_by_category_);
    for (auto& row : rows_) {
        if (row.is_header) row.header = gesture_category_title(row.gesture_category);
    }

    list_control_.set_category_chips(!group_by_category_);
    list_control_.set_rows(&rows_);
    list_control_.refresh();

    if (previous >= 0 && previous < static_cast<int>(settings_.bindings.size())) {
        select_binding(previous);
    }
    updating_ = was_updating;

    if (count_label_ != nullptr) {
        std::wstring text = L"Bindings: " + std::to_wstring(settings_.bindings.size()) + L" - ";
        if (sort.column == GestureSortColumn::none) {
            text += group_by_category_ ? L"Manual order within groups" : L"Manual order";
        } else {
            switch (sort.column) {
            case GestureSortColumn::gesture: text += L"Gesture"; break;
            case GestureSortColumn::command: text += L"Command"; break;
            case GestureSortColumn::place: text += L"Place"; break;
            default: break;
            }
            text += sort.direction == GestureSortDirection::ascending ? L" ascending" : L" descending";
        }
        SetWindowTextW(count_label_, text.c_str());
    }

    if (!list_diagnostics_logged_) {
        list_diagnostics_logged_ = true;
        FB2K_console_formatter() << "Mouse Gesture Host: table rows " << static_cast<t_size>(rows_.size())
                                 << ", bindings " << static_cast<t_size>(settings_.bindings.size());
    }

    update_buttons();
}

void GesturePreferencesInstance::refresh_gesture_text() {
    const std::wstring text = wide_from_utf8(format_gesture(editor_.draft.gesture, settings_.alphabet));
    SetWindowTextW(gesture_edit_, text.c_str());
}

void GesturePreferencesInstance::refresh_command_list() {
    if (command_combo_ == nullptr) return;

    const std::string group_id = current_group_id();
    catalog_ = list_commands(group_id);

    if (!diagnostics_logged_) {
        diagnostics_logged_ = true;
        FB2K_console_formatter() << "Mouse Gesture Host: " << static_cast<t_size>(settings_.bindings.size())
                                 << " gesture bindings, " << static_cast<t_size>(catalog_.size())
                                 << " commands in group \"" << group_id.c_str() << "\"";
    }

    if (catalog_.empty() && group_id == "main") {
        FB2K_console_formatter() << "Mouse Gesture Host: the main menu returned no commands "
                                    "(menu not ready?)";
    }

    // Пакетное обновление: не перерисовываем и не перераспределяем память
    // после каждой из сотен команд главного меню.
    SendMessageW(command_combo_, WM_SETREDRAW, FALSE, 0);
    ComboBox_ResetContent(command_combo_);
    std::size_t string_bytes = 0;
    for (const auto& entry : catalog_) {
        string_bytes += (entry.path.size() + 1) * sizeof(wchar_t);
    }
    SendMessageW(command_combo_, CB_INITSTORAGE, static_cast<WPARAM>(catalog_.size() + 1),
                 static_cast<LPARAM>(string_bytes));

    // Нулевой элемент не является командой. CurSel(-1) оставляет поле пустым,
    // пока пользователь явно не выберет executable identity.
    ComboBox_AddString(command_combo_, L"Select command…");
    for (const auto& entry : catalog_) {
        const std::wstring label = wide_from_utf8(entry.path);
        ComboBox_AddString(command_combo_, label.c_str());
    }
    ComboBox_SetCurSel(command_combo_, -1);

    SendMessageW(command_combo_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(command_combo_, nullptr, TRUE);
    update_buttons();
}

void GesturePreferencesInstance::update_buttons() {
    if (list_ == nullptr || add_button_ == nullptr) return;

    const int selected = selected_row();
    const int count = static_cast<int>(settings_.bindings.size());
    const bool has_selection = selected >= 0 && selected < count;
    const auto command = validate_editor_command(current_editor_binding(), catalog_, selected_command_index());
    const bool has_gesture = !editor_gesture().empty();
    const bool has_edit_target = editor_.mode == GestureEditorMode::edit && has_selection &&
                                 selected == editor_.binding_index;

    const bool valid_draft = has_gesture && command.is_selectable();
    const bool can_move = can_move_gesture_bindings(list_control_.sort_state(), group_by_category_);
    EnableWindow(add_button_, valid_draft);
    EnableWindow(update_button_, has_edit_target && editor_.dirty && valid_draft);
    EnableWindow(delete_button_, has_selection);
    EnableWindow(move_up_button_, can_move && has_selection && selected > 0);
    EnableWindow(move_down_button_, can_move && has_selection && selected + 1 < count);
    update_command_status();
}

void GesturePreferencesInstance::update_command_status() {
    if (command_choose_button_ == nullptr || command_retry_button_ == nullptr ||
        command_copy_button_ == nullptr) return;

    const auto status = validate_editor_command(current_editor_binding(), catalog_, selected_command_index());
    const std::wstring reason = status.message.empty()
        ? L"Command catalog did not provide additional details."
        : wide_from_utf8(status.message);
    std::wstring detail;
    std::wstring recovery;
    bool show_actions = false;

    switch (status.state) {
    case GestureEditorCommandState::valid:
        detail = L"[valid] Command selected explicitly.";
        recovery = L"Add/Update stages only this binding; Apply commits the whole Preferences page.";
        break;
    case GestureEditorCommandState::unavailable:
        detail = L"[unavailable] " + reason;
        show_actions = true;
        break;
    case GestureEditorCommandState::conflict:
        detail = L"[conflict] " + reason;
        show_actions = true;
        break;
    case GestureEditorCommandState::none:
        if (catalog_.empty()) {
            detail = current_group_id() == "main"
                ? L"[none] Main menu is not available yet."
                : L"[none] No context commands; select tracks first.";
            show_actions = true;
        } else {
            detail = L"[none] No command selected.";
            recovery = L"Select an available command explicitly; Add/Update remain disabled until then.";
        }
        break;
    }
    if (show_actions) {
        recovery = L"What to do: use «Выбрать другую» to pick a selectable command or «Повторить поиск». "
                   L"«Скопировать детали» copies the full diagnostic.";
    }

    std::wstring text = detail + L"\r\n" + recovery;

    bool layout_needed = false;
    if (text != command_status_text_) {
        command_status_text_ = std::move(text);
    }

    if (show_actions != command_actions_shown_) {
        command_actions_shown_ = show_actions;
        const int show = command_actions_shown_ ? SW_SHOW : SW_HIDE;
        ShowWindow(command_choose_button_, show);
        ShowWindow(command_retry_button_, show);
        ShowWindow(command_copy_button_, show);
        layout_needed = true;
    }

    bool has_selectable = false;
    for (const auto& entry : catalog_) {
        if (entry.selectable) {
            has_selectable = true;
            break;
        }
    }
    EnableWindow(command_choose_button_, command_actions_shown_ && has_selectable);
    EnableWindow(command_retry_button_, command_actions_shown_);
    EnableWindow(command_copy_button_, command_actions_shown_);

    if (layout_needed) layout();
}

// ---------------------------------------------------------------------------
//  Действия
// ---------------------------------------------------------------------------

void GesturePreferencesInstance::on_editor_changed() {
    if (updating_) return;
    refresh_editor_dirty(editor_);
    if (!editor_.dirty) pending_action_ = {};
    sync_page_dirty(true);
    update_buttons();
}

EditorGuardDecision GesturePreferencesInstance::show_editor_guard(const EditorPendingAction& action) {
    if (hwnd_ == nullptr || !IsWindow(hwnd_)) return EditorGuardDecision::cancel;

    const std::wstring content = std::wstring(L"Pending action: ") + editor_action_label(action.type) +
        L".\r\n\r\nSave stages Add/Update; Discard restores the baseline. "
        L"Apply remains the only commit of the whole Preferences page.";
    const TASKDIALOG_BUTTON buttons[] = {
        { IDYES, L"Save changes" },
        { IDNO, L"Discard changes" },
        { IDCANCEL, L"Отменить правку" },
    };

    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    const HWND root = GetAncestor(hwnd_, GA_ROOT);
    config.hwndParent = root != nullptr ? root : hwnd_;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"Mouse Gesture Host";
    config.pszMainInstruction = L"Save the unsaved binding changes?";
    config.pszContent = content.c_str();
    config.cButtons = 3;
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;
    config.pszMainIcon = TD_WARNING_ICON;

    int pressed = IDCANCEL;
    const HRESULT result = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    if (FAILED(result)) {
        FB2K_console_formatter() << "Mouse Gesture Host: TaskDialogIndirect failed (HRESULT "
                                 << static_cast<long>(result) << "); draft action cancelled";
        return EditorGuardDecision::cancel;
    }
    if (pressed == IDYES) return EditorGuardDecision::save;
    if (pressed == IDNO) return EditorGuardDecision::discard;
    return EditorGuardDecision::cancel;
}

void GesturePreferencesInstance::request_editor_action(const EditorPendingAction& action) {
    if (action.type == EditorActionType::none || editor_guard_active_) return;
    if (!editor_.dirty) {
        execute_editor_action(action);
        return;
    }

    pending_action_ = action;
    editor_guard_active_ = true;
    const EditorGuardDecision decision = show_editor_guard(action);
    editor_guard_active_ = false;

    // Во время modal loop хост мог уничтожить окно вопреки busy. Session уже
    // содержит draft; не обращаемся к уничтоженным controls.
    if (hwnd_ == nullptr || !IsWindow(hwnd_)) {
        pending_action_ = {};
        return;
    }

    switch (decision) {
    case EditorGuardDecision::save: on_save_editor(); break;
    case EditorGuardDecision::discard: on_discard_editor(); break;
    case EditorGuardDecision::cancel: on_cancel_editor_action(); break;
    }
}

void GesturePreferencesInstance::execute_editor_action(const EditorPendingAction& action) {
    switch (action.type) {
    case EditorActionType::select:
        select_binding(action.binding_index);
        load_binding_editor(action.binding_index);
        break;
    case EditorActionType::add: on_add(); break;
    case EditorActionType::new_binding: on_new(); break;
    case EditorActionType::reset: reset(); break;
    case EditorActionType::apply: apply(); break;
    case EditorActionType::remove: on_delete(); break;
    case EditorActionType::move: on_move(action.delta); break;
    case EditorActionType::none: break;
    }
}

bool GesturePreferencesInstance::commit_editor_changes() {
    if (editor_.mode == GestureEditorMode::new_binding) return on_add();
    if (editor_.mode == GestureEditorMode::edit) return on_update();
    return false;
}

void GesturePreferencesInstance::on_save_editor() {
    if (!editor_.dirty) return;

    const EditorPendingAction action = pending_action_;
    pending_action_ = {};
    if (commit_editor_changes()) {
        execute_editor_action(action);
    } else {
        pending_action_ = action;
        update_buttons();
    }
}

void GesturePreferencesInstance::on_discard_editor() {
    if (!editor_.dirty) return;

    const EditorPendingAction action = pending_action_;
    pending_action_ = {};
    discard_editor_changes(editor_);
    sync_editor_controls();
    sync_page_dirty();
    execute_editor_action(action);
}

void GesturePreferencesInstance::on_cancel_editor_action() {
    pending_action_ = {};
    update_buttons();
}

void GesturePreferencesInstance::on_group_changed() {
    if (updating_) return;
    refresh_command_list();
    const int command_index = change_editor_group(editor_.draft, current_group_id(), catalog_);
    ComboBox_SetCurSel(command_combo_, command_index < 0 ? -1 : command_index + 1);
    on_editor_changed();
}

void GesturePreferencesInstance::on_command_changed() {
    if (updating_) return;
    select_editor_command(editor_.draft, catalog_, selected_command_index());
    on_editor_changed();
}

void GesturePreferencesInstance::on_place_changed() {
    if (updating_) return;
    editor_.draft.place = current_place_id();
    on_editor_changed();
}

void GesturePreferencesInstance::on_command_choose_another() {
    if (!IsWindowEnabled(command_choose_button_) || command_combo_ == nullptr) return;
    SetFocus(command_combo_);
    ComboBox_ShowDropdown(command_combo_, TRUE);
}

void GesturePreferencesInstance::on_command_retry() {
    if (updating_ || !IsWindowEnabled(command_retry_button_)) return;

    // Sync разрешает только identity из draft. При пустом draft CurSel остаётся
    // -1; retry никогда не превращается в неявный выбор первой команды.
    sync_editor_controls();
    const auto status = validate_editor_command(current_editor_binding(), catalog_, selected_command_index());
    FB2K_console_formatter() << "Mouse Gesture Host: command search retried in group \""
                             << current_group_id().c_str() << "\", commands="
                             << static_cast<t_size>(catalog_.size()) << ", state="
                             << static_cast<int>(status.state);
}

std::wstring GesturePreferencesInstance::command_details_text() const {
    const auto value_or_none = [](const std::string& value) {
        return value.empty() ? std::wstring(L"(none)") : wide_from_utf8(value);
    };

    std::wstring details = L"Mouse Gesture Host command diagnostic\r\n";
    details += L"Gesture: " + value_or_none(editor_.draft.gesture) + L"\r\n";
    details += L"Group: " + value_or_none(editor_.draft.group) + L"\r\n";
    details += L"Command path: " + value_or_none(editor_.draft.command_path) + L"\r\n";
    details += L"Place: " + value_or_none(editor_.draft.place) + L"\r\n";
    details += L"Command GUID: " + value_or_none(editor_.draft.command_guid) + L"\r\n";
    details += L"Subcommand GUID: " + value_or_none(editor_.draft.subcommand_guid) + L"\r\n";
    details += L"Node path: " + value_or_none(editor_.draft.node_path) + L"\r\n";
    details += L"\r\n" + command_status_text_;
    return details;
}

void GesturePreferencesInstance::on_command_copy_details() {
    if (hwnd_ == nullptr || !IsWindowEnabled(command_copy_button_)) return;
    const std::wstring details = command_details_text();
    if (!OpenClipboard(hwnd_)) {
        FB2K_console_formatter() << "Mouse Gesture Host: could not open clipboard for command details";
        return;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (details.size() + 1) * sizeof(wchar_t));
    bool copied = false;
    if (memory != nullptr) {
        auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
        if (destination != nullptr) {
            std::wmemcpy(destination, details.c_str(), details.size() + 1);
            GlobalUnlock(memory);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                copied = true;
                memory = nullptr; // Владение передано системному clipboard.
            }
        }
    }
    CloseClipboard();

    if (memory != nullptr) GlobalFree(memory);
    if (!copied) {
        FB2K_console_formatter() << "Mouse Gesture Host: could not copy command details to clipboard";
        return;
    }
    FB2K_console_formatter() << "Mouse Gesture Host: command details copied to clipboard";
}

bool GesturePreferencesInstance::on_list_selection_guard(int binding_index) {
    if (updating_ || !editor_.dirty) return true;
    request_editor_action({ EditorActionType::select, binding_index });
    return false;
}

void GesturePreferencesInstance::on_list_selection_changed(int binding_index) {
    if (updating_) return;
    load_binding_editor(binding_index);
}

void GesturePreferencesInstance::on_new() {
    if (updating_) return;
    if (editor_.dirty) {
        request_editor_action({ EditorActionType::new_binding });
        return;
    }
    load_new_editor();
}

bool GesturePreferencesInstance::on_add() {
    if (updating_) return false;
    if (editor_.mode == GestureEditorMode::edit && editor_.dirty) {
        request_editor_action({ EditorActionType::add });
        return false;
    }

    const std::string gesture = editor_gesture();
    FB2K_console_formatter() << "Mouse Gesture Host: Add New pressed, gesture=\"" << gesture.c_str()
                             << "\", commands=" << static_cast<t_size>(catalog_.size());

    if (gesture.empty()) {
        MessageBoxW(hwnd_,
                    L"Build a gesture first using the direction, wheel, or rocker "
                    L"buttons on the left side of the page.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    // Список команд мог не построиться при открытии страницы (меню ещё не готово) -
    // пробуем ещё раз, прежде чем отказывать пользователю.
    if (catalog_.empty()) refresh_command_list();

    GestureBinding binding = current_editor_binding();
    const auto command = validate_editor_command(binding, catalog_, selected_command_index());
    if (!command.is_selectable()) {
        MessageBoxW(hwnd_, L"Select an available, non-conflicting command explicitly.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    select_editor_command(binding, catalog_, command.catalog_index);

    if (has_exact_binding_duplicate(settings_.bindings, binding)) {
        const int answer = MessageBoxW(hwnd_,
                                       L"An identical gesture, group, command, and place already exist.\n\n"
                                       L"Create a confirmed duplicate?",
                                       L"Mouse Gesture Host", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDYES) return false;
    }

    settings_.bindings.push_back(binding);
    set_dirty(true);
    refresh_list();

    const int last = static_cast<int>(settings_.bindings.size()) - 1;
    select_binding(last);
    load_binding_editor(last);

    FB2K_console_formatter() << "Mouse Gesture Host: added gesture \"" << binding.gesture.c_str()
                             << "\" -> " << binding.command_path.c_str()
                             << " (" << static_cast<t_size>(settings_.bindings.size()) << " total)";

    update_buttons();
    return true;
}

bool GesturePreferencesInstance::on_update() {
    if (updating_ || editor_.mode != GestureEditorMode::edit || !editor_.dirty) return false;
    const int index = editor_.binding_index;
    if (index < 0 || index >= static_cast<int>(settings_.bindings.size()) || selected_row() != index) {
        return false;
    }

    const std::string gesture = editor_gesture();
    if (gesture.empty()) {
        MessageBoxW(hwnd_, L"Build a gesture first using the buttons on the left.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    GestureBinding binding = current_editor_binding();
    const auto command = validate_editor_command(binding, catalog_, selected_command_index());
    if (!command.is_selectable()) {
        MessageBoxW(hwnd_, L"Select an available, non-conflicting command explicitly.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    select_editor_command(binding, catalog_, command.catalog_index);

    if (has_exact_binding_duplicate(settings_.bindings, binding, index)) {
        const int answer = MessageBoxW(hwnd_,
                                       L"Update will duplicate another gesture, group, command, and place.\n\n"
                                       L"Apply the confirmed duplicate?",
                                       L"Mouse Gesture Host", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDYES) return false;
    }

    settings_.bindings[static_cast<std::size_t>(index)] = binding;
    set_dirty(true);
    refresh_list();
    select_binding(index);
    load_binding_editor(index);
    return true;
}

void GesturePreferencesInstance::on_delete() {
    if (updating_) return;
    if (editor_.dirty) {
        request_editor_action({ EditorActionType::remove });
        return;
    }

    const int index = selected_row();
    if (index < 0 || index >= static_cast<int>(settings_.bindings.size())) return;

    settings_.bindings.erase(settings_.bindings.begin() + index);
    set_dirty(true);
    refresh_list();

    const int new_index = index < static_cast<int>(settings_.bindings.size())
        ? index
        : static_cast<int>(settings_.bindings.size()) - 1;
    if (new_index >= 0) {
        select_binding(new_index);
        load_binding_editor(new_index);
    } else {
        load_new_editor();
    }

    update_buttons();
}

void GesturePreferencesInstance::on_move(int delta) {
    if (updating_) return;
    const GestureSortState sort = list_control_.sort_state();
    if (!can_move_gesture_bindings(sort, group_by_category_)) return;
    if (editor_.dirty) {
        request_editor_action({ EditorActionType::move, -1, delta });
        return;
    }

    const int index = selected_row();
    if (!move_gesture_binding(settings_.bindings, index, delta, sort, group_by_category_)) return;
    const int target = index + delta;
    set_dirty(true);

    refresh_list();
    select_binding(target);
    load_binding_editor(target);
    update_buttons();
}

void GesturePreferencesInstance::on_direction(Token token) {
    if (updating_) return;

    std::vector<Token> tokens = editor_tokens();
    tokens.push_back(token);
    editor_.draft.gesture = join_tokens(tokens);
    refresh_gesture_text();
    on_editor_changed();
}

void GesturePreferencesInstance::on_backspace() {
    if (updating_) return;

    std::vector<Token> tokens = editor_tokens();
    if (!tokens.empty()) tokens.pop_back();
    editor_.draft.gesture = join_tokens(tokens);
    refresh_gesture_text();
    on_editor_changed();
}

std::vector<Token> GesturePreferencesInstance::editor_tokens() const {
    std::vector<Token> tokens;
    parse_gesture(editor_.draft.gesture, tokens);
    return tokens;
}

std::string GesturePreferencesInstance::editor_gesture() const {
    return normalize_gesture(editor_.draft.gesture);
}

void GesturePreferencesInstance::on_settings_control_changed() {
    if (updating_) return;
    set_dirty(true);
}

// ---------------------------------------------------------------------------
//  Вспомогательные методы
// ---------------------------------------------------------------------------

void GesturePreferencesInstance::set_dirty(bool dirty) {
    committed_dirty_ = dirty;
    sync_page_dirty();
}

void GesturePreferencesInstance::sync_page_dirty(bool force_notify) {
    if (editor_.dirty) {
        g_editor_draft_session = editor_;
    } else {
        g_editor_draft_session.reset();
    }

    const bool combined_dirty = committed_dirty_ || editor_.dirty;
    if (!force_notify && dirty_ == combined_dirty && notified_editor_busy_ == editor_.dirty) return;

    dirty_ = combined_dirty;
    notified_editor_busy_ = editor_.dirty;
    if (callback_.is_valid()) callback_->on_state_changed();
}

int GesturePreferencesInstance::current_group_index() const {
    const int index = ComboBox_GetCurSel(group_combo_);
    return index >= 0 ? index : 0;
}

int GesturePreferencesInstance::current_place_index() const {
    const int index = ComboBox_GetCurSel(place_combo_);
    return index >= 0 ? index : 0;
}

std::string GesturePreferencesInstance::current_group_id() const {
    const auto& groups = command_groups();
    const int index = current_group_index();
    if (index >= 0 && index < static_cast<int>(groups.size())) return groups[static_cast<std::size_t>(index)].first;
    return "main";
}

std::string GesturePreferencesInstance::current_place_id() const {
    const auto& places = command_places();
    const int index = current_place_index();
    if (index >= 0 && index < static_cast<int>(places.size())) return places[static_cast<std::size_t>(index)].first;
    return "main_window";
}

int GesturePreferencesInstance::selected_command_index() const {
    if (command_combo_ == nullptr) return -1;
    const int combo_index = ComboBox_GetCurSel(command_combo_);
    return combo_index > 0 ? combo_index - 1 : -1;
}

GestureBinding GesturePreferencesInstance::current_editor_binding() const {
    return editor_.draft;
}

int GesturePreferencesInstance::binding_index_from_display(int display_row) const {
    if (display_row < 0 || display_row >= static_cast<int>(rows_.size())) return -1;
    return rows_[static_cast<std::size_t>(display_row)].binding_index;
}

int GesturePreferencesInstance::display_row_from_binding_index(int binding_index) const {
    return gesture_row_for_binding(rows_, binding_index);
}

void GesturePreferencesInstance::select_binding(int binding_index) {
    if (list_ == nullptr) return;
    list_control_.select_binding(binding_index);
}

int GesturePreferencesInstance::selected_row() const {
    if (list_ == nullptr) return -1;
    return list_control_.selected_binding();
}

int GesturePreferencesInstance::read_stroke_length() const {
    const std::wstring text = read_window_text(stroke_edit_);
    if (text.empty()) return 8;

    wchar_t* end = nullptr;
    const long value = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str()) return 8;
    return clamp_int(static_cast<int>(value), kMinStrokeLength, kMaxStrokeLength);
}

// ---------------------------------------------------------------------------
//  Отложенная прокрутка длинного списка команд
// ---------------------------------------------------------------------------

void GesturePreferencesInstance::install_command_list_scroll_handler() {
    if (command_combo_ == nullptr) return;

    COMBOBOXINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(command_combo_, &info) || info.hwndList == nullptr ||
        command_list_ == info.hwndList) {
        return;
    }

    if (SetWindowSubclass(info.hwndList, command_list_subclass_proc, kCommandListSubclassId,
                          reinterpret_cast<DWORD_PTR>(this))) {
        command_list_ = info.hwndList;
    }
}

LRESULT CALLBACK GesturePreferencesInstance::command_list_subclass_proc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR subclass_id, DWORD_PTR ref_data) {
    if (msg == WM_VSCROLL && LOWORD(wparam) == SB_THUMBTRACK) {
        // Стандартный LISTBOX должен получить track-сообщение: его оконная
        // процедура обновляет top index и видимые строки до отпускания ползунка.
        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }

    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, command_list_subclass_proc, subclass_id);
        auto* self = reinterpret_cast<GesturePreferencesInstance*>(ref_data);
        if (self != nullptr) self->command_list_ = nullptr;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK GesturePreferencesInstance::help_button_subclass_proc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR subclass_id, DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<GesturePreferencesInstance*>(ref_data);

    switch (msg) {
    case WM_SETFOCUS:
        if (self != nullptr) self->on_help();
        break;
    case WM_KILLFOCUS:
    case WM_MOUSELEAVE:
        if (self != nullptr) self->hide_tracking_help();
        break;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tracking = {};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = hwnd;
        TrackMouseEvent(&tracking);
        break;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && self != nullptr && self->help_visible_) {
            self->hide_tracking_help();
            return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, help_button_subclass_proc, subclass_id);
        if (self != nullptr) self->help_button_ = nullptr;
        break;
    default:
        break;
    }

    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
//  Оконная процедура
// ---------------------------------------------------------------------------

LRESULT CALLBACK GesturePreferencesInstance::window_proc_static(HWND hwnd, UINT msg, WPARAM wparam,
                                                               LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* self = reinterpret_cast<GesturePreferencesInstance*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) self->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    auto* self = reinterpret_cast<GesturePreferencesInstance*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->window_proc(msg, wparam, lparam);
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT GesturePreferencesInstance::window_proc(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;

    case WM_SHOWWINDOW:
        // Хост не всегда присылает WM_SIZE после показа страницы, поэтому
        // подгоняем размер под клиентскую область родителя при показе.
        if (wparam != 0) {
            fit_to_parent();
            layout();
            // Меню могло быть недоступно в момент создания страницы.
            if (catalog_.empty()) refresh_command_list();

            if (pending_state_notify_) {
                pending_state_notify_ = false;
                if (callback_.is_valid()) callback_->on_state_changed();
            }
        }
        break;

    case WM_ERASEBKGND: {
        if (background_brush_ == nullptr) break;
        RECT rc = {};
        GetClientRect(hwnd_, &rc);
        FillRect(reinterpret_cast<HDC>(wparam), &rc, background_brush_);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        if (background_brush_ == nullptr) break;
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, text_color_);
        SetBkColor(dc, background_color_);
        return reinterpret_cast<LRESULT>(background_brush_);
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        if (!(dark_mode_ ? true : false) || control_brush_ == nullptr) break;
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(235, 235, 235));
        SetBkColor(dc, RGB(45, 45, 45));
        return reinterpret_cast<LRESULT>(control_brush_);
    }

    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (draw->CtlType == ODT_BUTTON && draw->CtlID == kIdHelp) {
            HBRUSH background = (draw->itemState & ODS_SELECTED) != 0 && control_brush_ != nullptr
                ? control_brush_
                : background_brush_;
            if (background == nullptr) background = GetSysColorBrush(COLOR_BTNFACE);
            FillRect(draw->hDC, &draw->rcItem, background);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, (draw->itemState & ODS_DISABLED) != 0
                ? GetSysColor(COLOR_GRAYTEXT)
                : text_color_);
            HGDIOBJ old_font = SelectObject(draw->hDC, font_);
            RECT marker = draw->rcItem;
            DrawTextW(draw->hDC, L"?", -1, &marker,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(draw->hDC, old_font);
            if ((draw->itemState & ODS_FOCUS) != 0) {
                RECT focus = draw->rcItem;
                InflateRect(&focus, -scale(2), -scale(2));
                DrawFocusRect(draw->hDC, &focus);
            }
            return TRUE;
        }
        break;
    }

    case WM_PARENTNOTIFY: {
        const UINT child_message = LOWORD(wparam);
        if (child_message == WM_LBUTTONDOWN || child_message == WM_RBUTTONDOWN) {
            const HWND clicked = reinterpret_cast<HWND>(lparam);
            if (clicked != help_button_) hide_tracking_help();
        }
        break;
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        hide_tracking_help();
        break;

    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && help_visible_) {
            hide_tracking_help();
            return 0;
        }
        break;

    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int code = HIWORD(wparam);
        const bool is_combo = id == kIdGroupCombo || id == kIdCommandCombo || id == kIdPlaceCombo;

        if (is_combo && code == CBN_DROPDOWN) {
            if (id == kIdCommandCombo) install_command_list_scroll_handler();
            // Не даём tooltip перекрыть раскрытый список или появиться поверх него.
            set_tooltips_active(false);
            return 0;
        }
        if (is_combo && code == CBN_CLOSEUP) {
            set_tooltips_active(true);
            return 0;
        }
        if (help_visible_ && id != kIdHelp) hide_tracking_help();

        switch (id) {
        case kIdDirUp: if (code == BN_CLICKED) on_direction(Token::up); return 0;
        case kIdDirDown: if (code == BN_CLICKED) on_direction(Token::down); return 0;
        case kIdDirLeft: if (code == BN_CLICKED) on_direction(Token::left); return 0;
        case kIdDirRight: if (code == BN_CLICKED) on_direction(Token::right); return 0;
        case kIdDirUpLeft: if (code == BN_CLICKED) on_direction(Token::up_left); return 0;
        case kIdDirUpRight: if (code == BN_CLICKED) on_direction(Token::up_right); return 0;
        case kIdDirDownLeft: if (code == BN_CLICKED) on_direction(Token::down_left); return 0;
        case kIdDirDownRight: if (code == BN_CLICKED) on_direction(Token::down_right); return 0;
        case kIdDirBackspace: if (code == BN_CLICKED) on_backspace(); return 0;

        case kIdWheelUp: if (code == BN_CLICKED) on_direction(Token::wheel_up); return 0;
        case kIdWheelDown: if (code == BN_CLICKED) on_direction(Token::wheel_down); return 0;
        case kIdRockerRightLeft: if (code == BN_CLICKED) on_direction(Token::rocker_right_left); return 0;
        case kIdRockerLeftRight: if (code == BN_CLICKED) on_direction(Token::rocker_left_right); return 0;

        case kIdGroupCombo: if (code == CBN_SELCHANGE) on_group_changed(); return 0;
        case kIdCommandCombo: if (code == CBN_SELCHANGE) on_command_changed(); return 0;
        case kIdPlaceCombo: if (code == CBN_SELCHANGE) on_place_changed(); return 0;
        case kIdCommandChooseAnother: if (code == BN_CLICKED) on_command_choose_another(); return 0;
        case kIdCommandRetry: if (code == BN_CLICKED) on_command_retry(); return 0;
        case kIdCommandCopyDetails: if (code == BN_CLICKED) on_command_copy_details(); return 0;

        case kIdNew: if (code == BN_CLICKED) on_new(); return 0;
        case kIdAdd: if (code == BN_CLICKED) on_add(); return 0;
        case kIdUpdate: if (code == BN_CLICKED) on_update(); return 0;
        case kIdDelete: if (code == BN_CLICKED) on_delete(); return 0;
        case kIdMoveUp: if (code == BN_CLICKED) on_move(-1); return 0;
        case kIdMoveDown: if (code == BN_CLICKED) on_move(1); return 0;
        case kIdGroupByCategory:
            if (code == BN_CLICKED) {
                group_by_category_ = Button_GetCheck(group_by_category_check_) == BST_CHECKED;
                refresh_list();
            }
            return 0;
        case kIdHelp: if (code == BN_CLICKED) on_help(); return 0;

        case kIdStrokeEdit: if (code == EN_CHANGE) on_settings_control_changed(); return 0;
        case kIdDisable:
        case kIdAlphabet:
        case kIdFirstOnly:
            if (code == BN_CLICKED) {
                settings_.alphabet = Button_GetCheck(alphabet_check_) == BST_CHECKED;
                // Перерисовываем жесты в новом представлении сразу.
                refresh_list();
                refresh_gesture_text();
                on_settings_control_changed();
            }
            return 0;

        default:
            break;
        }
        break;
    }

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        apply_theme();
        layout();
        break;

    case WM_NCDESTROY: {
        HWND dying = hwnd_;
        SetWindowLongPtrW(dying, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        return DefWindowProcW(dying, msg, wparam, lparam);
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
//  preferences_page_v3
// ---------------------------------------------------------------------------

const char* GesturePreferencesPage::get_name() {
    return "Mouse Gesture Host";
}

GUID GesturePreferencesPage::get_guid() {
    return kPreferencesPageGuid;
}

GUID GesturePreferencesPage::get_parent_guid() {
    return preferences_page::guid_tools;
}

preferences_page_instance::ptr GesturePreferencesPage::instantiate(fb2k::hwnd_t parent,
                                                                  preferences_page_callback::ptr callback) {
    return fb2k::service_new<GesturePreferencesInstance>(static_cast<HWND>(parent), callback);
}

// Регистрация страницы в дереве настроек foobar2000.
static preferences_page_factory_t<GesturePreferencesPage> g_preferences_page_factory;

} // namespace mgh
