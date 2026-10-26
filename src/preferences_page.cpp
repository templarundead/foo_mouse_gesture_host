#include "preferences_page.h"

#include "command_catalog.h"
#include "gesture_engine.h"
#include "gesture_list_sort.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cwchar>
#include <functional>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace mgh {
namespace {

constexpr GUID kPreferencesPageGuid = { 0x6f0c4a52, 0x8d31, 0x4c0e, { 0x9b, 0x7e, 0x2a, 0x64, 0x11, 0xd8, 0x3f, 0x10 } };
constexpr wchar_t kPreferencesPageClass[] = L"foo_mouse_gesture_host_preferences";
constexpr UINT_PTR kCommandListSubclassId = 0x4d4748;
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
    kIdAdd,
    kIdUpdate,
    kIdDelete,
    kIdMoveUp,
    kIdMoveDown,
    kIdStrokeEdit,
    kIdStrokeSpin,
    kIdDisable,
    kIdAlphabet,
    kIdFirstOnly,
    kIdHelp
};

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

int measure_wrapped_text_height(HWND hwnd, HFONT font, const wchar_t* text, int width) {
    if (hwnd == nullptr || font == nullptr || text == nullptr || width <= 0) return 0;
    HDC dc = GetDC(hwnd);
    if (dc == nullptr) return 0;

    HGDIOBJ old_font = SelectObject(dc, font);
    RECT bounds{0, 0, width, 0};
    const int height = DrawTextW(dc, text, -1, &bounds,
                                 DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old_font);
    ReleaseDC(hwnd, dc);
    return height;
}

const wchar_t* gesture_category_title(GestureCategory category) {
    switch (category) {
    case GestureCategory::wheel_up: return L"Wheel Up";
    case GestureCategory::wheel_down: return L"Wheel Down";
    case GestureCategory::rocker: return L"Rocker";
    default: return L"Gesture";
    }
}

const wchar_t* gesture_category_cell_prefix(GestureCategory category) {
    switch (category) {
    case GestureCategory::wheel_up: return L"Wheel Up · ";
    case GestureCategory::wheel_down: return L"Wheel Down · ";
    case GestureCategory::rocker: return L"Rocker · ";
    default: return L"Gesture · ";
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
    void set_sort_handler(sort_handler handler) { on_sort_ = std::move(handler); }

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
        if (self != nullptr) return self->window_proc(msg, wparam, lparam);
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

        const int min_gesture = scale(60);
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
        case 0: return scale(60);
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
        if ((*rows_)[static_cast<std::size_t>(index)].is_header) return;

        selected_ = index;
        invalidate();

        if (notify && on_select_) on_select_((*rows_)[static_cast<std::size_t>(index)].binding_index);
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
    sort_handler on_sort_;
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
        const int page_rows = 5;
        switch (wparam) {
        case VK_UP: select_row(selected_ > 0 ? selected_ - 1 : 0, true); break;
        case VK_DOWN:
            if (rows_ != nullptr && selected_ + 1 < static_cast<int>(rows_->size())) {
                select_row(selected_ + 1, true);
            }
            break;
        case VK_PRIOR:
            for (int i = 0; i < page_rows; ++i) select_row(selected_ - 1, false);
            if (rows_ != nullptr) select_row(selected_, true);
            break;
        case VK_NEXT: {
            if (rows_ == nullptr) break;
            for (int i = 0; i < page_rows; ++i) select_row(selected_ + 1, false);
            select_row(selected_, true);
            break;
        }
        case VK_HOME: select_row(0, true); break;
        case VK_END:
            if (rows_ != nullptr) select_row(static_cast<int>(rows_->size()) - 1, true);
            break;
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
    void refresh_list();
    void refresh_gesture_text();
    void refresh_command_list();
    void update_buttons();

    void on_group_changed();
    void on_list_selection_changed(int index);
    void on_add();
    void on_update();
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

    // --- данные ---
    GestureSettings settings_;
    std::vector<CatalogEntry> catalog_;
    std::string edit_gesture_;

    // --- Win32 ---
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;

    //! Строки таблицы: заголовки групп и сами привязки.
    std::vector<GestureListRow> rows_;
    //! Собственный контрол таблицы (см. GestureListControl).
    GestureListControl list_control_;

    HWND count_label_ = nullptr;
    HWND hint_label_ = nullptr;
    HWND help_button_ = nullptr;
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
    bool updating_ = false;
    bool diagnostics_logged_ = false;
    bool pending_state_notify_ = false;
    bool list_diagnostics_logged_ = false;
    bool help_visible_ = false;

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
    return state;
}

fb2k::hwnd_t GesturePreferencesInstance::get_wnd() {
    return hwnd_;
}

void GesturePreferencesInstance::apply() {
    settings_.stroke_length = read_stroke_length();
    settings_.disabled = Button_GetCheck(disable_check_) == BST_CHECKED;
    settings_.alphabet = Button_GetCheck(alphabet_check_) == BST_CHECKED;
    settings_.first_only = Button_GetCheck(first_only_check_) == BST_CHECKED;

    GestureConfig::instance().apply(settings_);
    GestureEngine::instance().reload();

    set_dirty(false);
}

void GesturePreferencesInstance::reset() {
    settings_ = GestureConfig::defaults();
    load_from_settings();
    set_dirty(true);
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
        list_control_.set_select_handler([this](int binding_index) {
            on_list_selection_changed(binding_index);
            update_buttons();
        });
        list_control_.set_sort_handler([this] {
            refresh_list();
        });
    }

    count_label_ = make_static(L"", SS_LEFT | SS_CENTERIMAGE);
    help_button_ = make_button(L"Help", kIdHelp);
    hint_label_ = make_static(kPreferencesHelp, SS_LEFT | SS_NOPREFIX);

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

    add_button_ = make_button(L"Add New", kIdAdd);
    update_button_ = make_button(L"Update", kIdUpdate);
    delete_button_ = make_button(L"Delete", kIdDelete);
    move_up_button_ = make_button(L"Up", kIdMoveUp);
    move_down_button_ = make_button(L"Down", kIdMoveDown);

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
    add_tooltip(place_combo_, L"Place: choose the execution context for the command.");

    add_tooltip(add_button_, L"Add New: add the current Gesture and Command as a binding.");
    add_tooltip(update_button_, L"Update: replace the selected binding with the editor values.");
    add_tooltip(delete_button_, L"Delete: remove the selected binding.");
    add_tooltip(move_up_button_, L"Up: move the selected binding earlier in manual order.");
    add_tooltip(move_down_button_, L"Down: move the selected binding later in manual order.");
    add_tooltip(help_button_, kPreferencesHelp);

    // Отдельный tracking tool позволяет открыть ту же справку кнопкой Help
    // с клавиатуры, не создавая модальное окно внутри preferences instance.
    TOOLINFOW help_tool = {};
    help_tool.cbSize = sizeof(help_tool);
    help_tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    help_tool.hwnd = hwnd_;
    help_tool.uId = kHelpTrackingToolId;
    help_tool.lpszText = const_cast<wchar_t*>(kPreferencesHelp);
    GetClientRect(hwnd_, &help_tool.rect);
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&help_tool));
}

void GesturePreferencesInstance::hide_tracking_help() {
    if (tooltip_ != nullptr && IsWindow(tooltip_) && help_visible_) {
        TOOLINFOW help_tool = {};
        help_tool.cbSize = sizeof(help_tool);
        help_tool.hwnd = hwnd_;
        help_tool.uId = kHelpTrackingToolId;
        SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&help_tool));
    }
    help_visible_ = false;
}

void GesturePreferencesInstance::on_help() {
    if (tooltip_ == nullptr || help_button_ == nullptr) return;
    if (help_visible_) {
        hide_tracking_help();
        return;
    }

    SendMessageW(tooltip_, TTM_POP, 0, 0);
    RECT button_rect = {};
    GetWindowRect(help_button_, &button_rect);
    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0,
                 MAKELPARAM(button_rect.left, button_rect.bottom + scale(4)));

    TOOLINFOW help_tool = {};
    help_tool.cbSize = sizeof(help_tool);
    help_tool.hwnd = hwnd_;
    help_tool.uId = kHelpTrackingToolId;
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&help_tool));
    help_visible_ = true;
}

void GesturePreferencesInstance::set_tooltips_active(bool active) {
    if (tooltip_ == nullptr || !IsWindow(tooltip_)) return;
    if (!active) {
        hide_tracking_help();
        SendMessageW(tooltip_, TTM_POP, 0, 0);
    }
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

    const ButtonSpec button_specs[5] = {
        { add_button_, L"Add New", 96 },
        { update_button_, L"Update", 72 },
        { delete_button_, L"Delete", 72 },
        { move_up_button_, L"Up", 60 },
        { move_down_button_, L"Down", 60 },
    };

    const int button_padding = scale(14);
    int button_widths[5] = {};
    for (int i = 0; i < 5; ++i) {
        const int needed = text_width(button_specs[i].text) + 2 * button_padding;
        button_widths[i] = needed > scale(button_specs[i].min_width)
            ? needed
            : scale(button_specs[i].min_width);
    }

    // Раскладываем кнопки по рядам жадно: в ряд попадает столько кнопок,
    // сколько реально помещается в ширину формы. Так раскладка работает
    // при любом DPI и любой ширине страницы, а не только в 1-2 ряда.
    int button_row_of[5] = {};
    int button_rows = 1;
    {
        int used = 0;
        for (int i = 0; i < 5; ++i) {
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

    // --- правая колонка: 4 строки формы + кнопки + подсказка ----------------
    const int row_step = control_height + scale(6);
    const int full_hint_width = width - 2 * margin;
    const bool full_width_hint = form_width_early < scale(320);
    const int hint_width = full_width_hint ? full_hint_width : form_width_early;
    // Static и DrawText используют один шрифт и одинаковые правила переноса.
    const int hint_height = measure_wrapped_text_height(hwnd_, font_, kPreferencesHelp, hint_width) +
                            scale(4);
    const int right_controls_height = 4 * row_step + scale(8) +
                                      button_rows * control_height + (button_rows - 1) * gap +
                                      scale(6);
    const int right_content_height = right_controls_height + (full_width_hint ? 0 : hint_height);

    const int columns_height = left_content_height > right_content_height ? left_content_height
                                                                          : right_content_height;
    // На узкой странице подсказка занимает отдельную полосу под колонками.
    const int lower_height = columns_height + (full_width_hint ? hint_height : 0);

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

    // Счётчик и Help делят существующую строку, поэтому справка не отнимает
    // дополнительную высоту у таблицы.
    const int help_width = scale(52);
    const int count_width = list_width - help_width - gap;
    MoveWindow(count_label_, margin + scale(6), count_top, count_width, count_height, TRUE);
    MoveWindow(help_button_, width - margin - help_width, count_top,
               help_width, count_height, TRUE);

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

    MoveWindow(place_label_, form_x, lower_top + 3 * row_step + scale(4), label_width, label_height, TRUE);
    SetWindowPos(place_combo_, nullptr, editor_x, lower_top + 3 * row_step, editor_width, scale(160),
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // Кнопки управления таблицей. Ширина каждой уже посчитана по тексту
    // (button_widths выше), ряды - жадно по доступной ширине (button_row_of).
    const int buttons_y = lower_top + 4 * row_step + scale(8);

    int row_x[8] = {};
    for (int i = 0; i < button_rows; ++i) row_x[i] = form_x;

    for (int i = 0; i < 5; ++i) {
        const int row = button_row_of[i];
        const int y = buttons_y + row * (control_height + gap);
        MoveWindow(button_specs[i].hwnd, row_x[row], y, button_widths[i], control_height, TRUE);
        row_x[row] += button_widths[i] + gap;
    }

    const int buttons_bottom = buttons_y + button_rows * control_height + (button_rows - 1) * gap;

    if (hint_label_ != nullptr) {
        const int hint_x = full_width_hint ? margin : form_x;
        const int hint_y = full_width_hint ? lower_top + columns_height : buttons_bottom + scale(6);
        MoveWindow(hint_label_, hint_x, hint_y, hint_width, hint_height, TRUE);
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

    edit_gesture_.clear();
    refresh_gesture_text();
    refresh_list();
    refresh_command_list();

    updating_ = false;
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

    rows_.clear();
    if (sort.column == GestureSortColumn::none) {
        const wchar_t* previous_group_title = nullptr;
        rows_.reserve(manual_rows.size() + 4);
        for (auto& row : manual_rows) {
            const wchar_t* group_title = gesture_category_title(row.gesture_category);
            if (previous_group_title == nullptr || wcscmp(previous_group_title, group_title) != 0) {
                GestureListRow header;
                header.is_header = true;
                header.header = group_title;
                rows_.push_back(std::move(header));
                previous_group_title = group_title;
            }
            rows_.push_back(std::move(row));
        }
    } else {
        rows_ = make_gesture_list_view(manual_rows, sort);
        for (auto& row : rows_) {
            row.gesture = std::wstring(gesture_category_cell_prefix(row.gesture_category)) + row.gesture;
        }
    }

    list_control_.set_rows(&rows_);
    list_control_.refresh();

    if (previous >= 0 && previous < static_cast<int>(settings_.bindings.size())) {
        select_binding(previous);
    }
    updating_ = was_updating;

    if (count_label_ != nullptr) {
        std::wstring text = L"Bindings: " + std::to_wstring(settings_.bindings.size()) + L" - ";
        if (sort.column == GestureSortColumn::none) {
            text += L"Manual order";
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
    const std::wstring text = wide_from_utf8(format_gesture(edit_gesture_, settings_.alphabet));
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
    if (!catalog_.empty()) {
        std::size_t string_bytes = 0;
        for (const auto& entry : catalog_) {
            string_bytes += (entry.path.size() + 1) * sizeof(wchar_t);
        }
        SendMessageW(command_combo_, CB_INITSTORAGE, static_cast<WPARAM>(catalog_.size()),
                     static_cast<LPARAM>(string_bytes));
    }

    if (catalog_.empty()) {
        // Пустой список - не повод молча не работать кнопке Add New:
        // показываем причину прямо в выпадающем списке.
        const wchar_t* placeholder = group_id == "main"
            ? L"(main menu is not available yet)"
            : L"(nothing selected - pick tracks first)";
        ComboBox_AddString(command_combo_, placeholder);
        ComboBox_SetCurSel(command_combo_, -1);
    } else {
        for (const auto& entry : catalog_) {
            const std::wstring label = wide_from_utf8(entry.path);
            ComboBox_AddString(command_combo_, label.c_str());
        }
        ComboBox_SetCurSel(command_combo_, 0);
    }

    SendMessageW(command_combo_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(command_combo_, nullptr, TRUE);
    update_buttons();
}

void GesturePreferencesInstance::update_buttons() {
    if (list_ == nullptr || add_button_ == nullptr) return;

    const int selected = selected_row();
    const int count = static_cast<int>(settings_.bindings.size());
    const bool has_selection = selected >= 0 && selected < count;

    const int command_index = command_combo_ != nullptr ? ComboBox_GetCurSel(command_combo_) : -1;
    const bool has_command = !catalog_.empty() && command_index >= 0 &&
                             command_index < static_cast<int>(catalog_.size()) &&
                             catalog_[static_cast<std::size_t>(command_index)].selectable;

    EnableWindow(add_button_, has_command);
    EnableWindow(update_button_, has_selection && has_command);
    EnableWindow(delete_button_, has_selection);
    EnableWindow(move_up_button_, has_selection && selected > 0);
    EnableWindow(move_down_button_, has_selection && selected + 1 < count);
}

// ---------------------------------------------------------------------------
//  Действия
// ---------------------------------------------------------------------------

void GesturePreferencesInstance::on_group_changed() {
    if (updating_) return;
    refresh_command_list();
}

void GesturePreferencesInstance::on_list_selection_changed(int binding_index) {
    if (updating_) return;
    if (binding_index < 0 || binding_index >= static_cast<int>(settings_.bindings.size())) return;

    const int index = binding_index;

    const GestureBinding& binding = settings_.bindings[static_cast<std::size_t>(index)];

    updating_ = true;

    edit_gesture_ = binding.gesture;
    refresh_gesture_text();

    int group_index = 0;
    const auto& groups = command_groups();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (binding.group == groups[i].first) group_index = static_cast<int>(i);
    }
    ComboBox_SetCurSel(group_combo_, group_index);
    refresh_command_list();

    const CommandMatch match = resolve_command(binding, catalog_);
    int command_index = match.status == CommandResolution::found
        ? static_cast<int>(match.index)
        : -1;
    if (command_index < 0) {
        // Сохранённую команду показываем, но не делаем вид, что она исполняема.
        // Synthetic строка не участвует в Add/Update и не выбирает первый дубликат.
        CatalogEntry missing;
        missing.path = binding.command_path;
        missing.group = binding.group;
        missing.command_guid = binding.command_guid;
        missing.subcommand_guid = binding.subcommand_guid;
        missing.node_path = binding.node_path;
        missing.selectable = false;
        catalog_.push_back(missing);

        const char* status = match.status == CommandResolution::ambiguous
            ? "conflict" : "unavailable";
        const std::wstring label = wide_from_utf8(missing.path + " [" + status + "]");
        ComboBox_AddString(command_combo_, label.c_str());
        command_index = static_cast<int>(catalog_.size()) - 1;

        FB2K_console_formatter() << "Mouse Gesture Host: binding \"" << binding.gesture.c_str()
                                 << "\" -> \"" << binding.command_path.c_str() << "\" is " << status
                                 << ": " << match.message.c_str();
    }
    ComboBox_SetCurSel(command_combo_, command_index);

    int place_index = 0;
    const auto& places = command_places();
    for (std::size_t i = 0; i < places.size(); ++i) {
        if (binding.place == places[i].first) place_index = static_cast<int>(i);
    }
    ComboBox_SetCurSel(place_combo_, place_index);

    updating_ = false;
    update_buttons();
}

void GesturePreferencesInstance::on_add() {
    const std::string gesture = editor_gesture();

    FB2K_console_formatter() << "Mouse Gesture Host: Add New pressed, gesture=\"" << gesture.c_str()
                             << "\", commands=" << static_cast<t_size>(catalog_.size());

    if (gesture.empty()) {
        MessageBoxW(hwnd_,
                    L"Build a gesture first using the direction, wheel, or rocker "
                    L"buttons on the left side of the page.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Список команд мог не построиться при открытии страницы (меню ещё не готово) -
    // пробуем ещё раз, прежде чем отказывать пользователю.
    if (catalog_.empty()) refresh_command_list();

    int command_index = ComboBox_GetCurSel(command_combo_);
    if (command_index < 0 && !catalog_.empty()) command_index = 0;

    if (catalog_.empty() || command_index >= static_cast<int>(catalog_.size())) {
        MessageBoxW(hwnd_,
                    L"No command is selected.\n\n"
                    L"For context(...) groups, select tracks in the playlist so the command "
                    L"list can be built, or choose the \"main menu\" group.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const CatalogEntry& selected_entry = catalog_[static_cast<std::size_t>(command_index)];
    if (!selected_entry.selectable) {
        MessageBoxW(hwnd_, L"The selected command is unavailable or conflicting. Choose a valid command.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    GestureBinding binding;
    binding.gesture = gesture;
    binding.group = current_group_id();
    assign_command_identity(binding, selected_entry);
    binding.place = current_place_id();

    settings_.bindings.push_back(binding);
    refresh_list();

    const int last = static_cast<int>(settings_.bindings.size()) - 1;
    select_binding(last);

    FB2K_console_formatter() << "Mouse Gesture Host: added gesture \"" << binding.gesture.c_str()
                             << "\" -> " << binding.command_path.c_str()
                             << " (" << static_cast<t_size>(settings_.bindings.size()) << " total)";

    update_buttons();
    set_dirty(true);
}

void GesturePreferencesInstance::on_update() {
    const int index = selected_row();
    if (index < 0 || index >= static_cast<int>(settings_.bindings.size())) return;

    const std::string gesture = editor_gesture();
    if (gesture.empty()) {
        MessageBoxW(hwnd_, L"Build a gesture first using the buttons on the left.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int command_index = ComboBox_GetCurSel(command_combo_);
    if (catalog_.empty() || command_index < 0 || command_index >= static_cast<int>(catalog_.size())) {
        MessageBoxW(hwnd_, L"Select a command to update the binding.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const CatalogEntry& selected_entry = catalog_[static_cast<std::size_t>(command_index)];
    if (!selected_entry.selectable) {
        MessageBoxW(hwnd_, L"The selected command is unavailable or conflicting. Choose a valid command.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    GestureBinding binding;
    binding.gesture = gesture;
    binding.group = current_group_id();
    assign_command_identity(binding, selected_entry);
    binding.place = current_place_id();

    settings_.bindings[static_cast<std::size_t>(index)] = binding;
    refresh_list();
    select_binding(index);

    set_dirty(true);
}

void GesturePreferencesInstance::on_delete() {
    const int index = selected_row();
    if (index < 0 || index >= static_cast<int>(settings_.bindings.size())) return;

    settings_.bindings.erase(settings_.bindings.begin() + index);
    refresh_list();

    const int new_index = index < static_cast<int>(settings_.bindings.size())
        ? index
        : static_cast<int>(settings_.bindings.size()) - 1;
    if (new_index >= 0) select_binding(new_index);

    update_buttons();
    set_dirty(true);
}

void GesturePreferencesInstance::on_move(int delta) {
    const int index = selected_row();
    const int count = static_cast<int>(settings_.bindings.size());
    const int target = index + delta;

    if (index < 0 || index >= count || target < 0 || target >= count) return;

    if (list_control_.sort_state().column != GestureSortColumn::none) {
        list_control_.set_sort_state({});
        refresh_list();
        select_binding(index);
    }

    std::swap(settings_.bindings[static_cast<std::size_t>(index)],
              settings_.bindings[static_cast<std::size_t>(target)]);

    refresh_list();
    select_binding(target);
    update_buttons();
    set_dirty(true);
}

void GesturePreferencesInstance::on_direction(Token token) {
    if (updating_) return;

    std::vector<Token> tokens = editor_tokens();
    tokens.push_back(token);
    edit_gesture_ = join_tokens(tokens);
    refresh_gesture_text();
}

void GesturePreferencesInstance::on_backspace() {
    if (updating_) return;

    std::vector<Token> tokens = editor_tokens();
    if (!tokens.empty()) tokens.pop_back();
    edit_gesture_ = join_tokens(tokens);
    refresh_gesture_text();
}

std::vector<Token> GesturePreferencesInstance::editor_tokens() const {
    std::vector<Token> tokens;
    parse_gesture(utf8_from_wide(read_window_text(gesture_edit_).c_str()), tokens);
    return tokens;
}

std::string GesturePreferencesInstance::editor_gesture() const {
    return normalize_gesture(utf8_from_wide(read_window_text(gesture_edit_).c_str()));
}

void GesturePreferencesInstance::on_settings_control_changed() {
    if (updating_) return;
    set_dirty(true);
}

// ---------------------------------------------------------------------------
//  Вспомогательные методы
// ---------------------------------------------------------------------------

void GesturePreferencesInstance::set_dirty(bool dirty) {
    if (dirty_ == dirty) return;
    dirty_ = dirty;
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
    if (self != nullptr) return self->window_proc(msg, wparam, lparam);

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

        case kIdAdd: if (code == BN_CLICKED) on_add(); return 0;
        case kIdUpdate: if (code == BN_CLICKED) on_update(); return 0;
        case kIdDelete: if (code == BN_CLICKED) on_delete(); return 0;
        case kIdMoveUp: if (code == BN_CLICKED) on_move(-1); return 0;
        case kIdMoveDown: if (code == BN_CLICKED) on_move(1); return 0;
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
