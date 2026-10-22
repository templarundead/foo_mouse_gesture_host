#include "preferences_page.h"

#include "command_catalog.h"
#include "gesture_engine.h"

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

enum ControlId : int {
    kIdList = 1001,
    kIdDirUp,
    kIdDirDown,
    kIdDirLeft,
    kIdDirRight,
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
    kIdFirstOnly
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

struct GestureListRow {
    bool is_header = false;
    int binding_index = -1;
    std::wstring header;
    std::wstring gesture;
    std::wstring command;
    std::wstring place;
};

class GestureListControl {
public:
    using select_handler = std::function<void(int binding_index)>;

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
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
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

    void set_columns(int gesture, int command, int place) {
        column_gesture_ = gesture;
        column_command_ = command;
        column_place_ = place;
        invalidate();
    }

    void set_select_handler(select_handler handler) { on_select_ = std::move(handler); }

    int selected_binding() const {
        if (rows_ == nullptr || selected_ < 0 || selected_ >= static_cast<int>(rows_->size())) return -1;
        return (*rows_)[static_cast<std::size_t>(selected_)].binding_index;
    }

    void select_binding(int binding_index) {
        if (rows_ == nullptr) return;

        for (std::size_t i = 0; i < rows_->size(); ++i) {
            if ((*rows_)[i].binding_index == binding_index) {
                selected_ = static_cast<int>(i);
                scroll_to_row(selected_);
                invalidate();
                return;
            }
        }
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
        if (GetTextMetricsW(dc, &metrics)) row_height_ = static_cast<int>(metrics.tmHeight) + scale(8);

        SelectObject(dc, header_font_ != nullptr ? header_font_ : font_);
        if (GetTextMetricsW(dc, &metrics)) header_height_ = static_cast<int>(metrics.tmHeight) + scale(12);

        SelectObject(dc, old_font);
        ReleaseDC(hwnd_, dc);

        if (row_height_ < scale(18)) row_height_ = scale(18);
        if (header_height_ < scale(22)) header_height_ = scale(22);

        update_scroll_info();
    }

    int content_height() const {
        if (rows_ == nullptr) return 0;

        int total = 0;
        for (const auto& row : *rows_) total += row_height(row.is_header);
        return total;
    }

    void update_scroll_info() {
        if (hwnd_ == nullptr) return;

        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int page = client.bottom - client.top;
        const int total = content_height();

        SCROLLINFO info = {};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = total > 0 ? total - 1 : 0;
        info.nPage = static_cast<UINT>(page > 0 ? page : 0);
        info.nPos = scroll_;
        SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);

        clamp_scroll();
    }

    void clamp_scroll() {
        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int page = client.bottom - client.top;
        const int max_scroll = content_height() - page;
        if (scroll_ > max_scroll) scroll_ = max_scroll;
        if (scroll_ < 0) scroll_ = 0;
    }

    void scroll_to(int position) {
        scroll_ = position;
        clamp_scroll();
        update_scroll_info();
        invalidate();
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
        const int page = client.bottom - client.top;

        if (top < scroll_) scroll_to(top);
        else if (top + height > scroll_ + page) scroll_to(top + height - page);
    }

    int row_at(int client_y) const {
        if (rows_ == nullptr) return -1;

        int y = -scroll_;
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
    void draw_row(HDC dc, const RECT& row_rect, const GestureListRow& row, bool selected);

    LRESULT window_proc(UINT msg, WPARAM wparam, LPARAM lparam);

    HWND hwnd_ = nullptr;
    std::vector<GestureListRow>* rows_ = nullptr;

    HFONT font_ = nullptr;
    HFONT header_font_ = nullptr;
    int dpi_ = 96;

    int row_height_ = 22;
    int header_height_ = 26;

    int column_gesture_ = 160;
    int column_command_ = 320;
    int column_place_ = 120;

    int scroll_ = 0;
    int selected_ = -1;
    bool focused_ = false;

    COLORREF background_ = RGB(32, 32, 32);
    COLORREF text_ = RGB(230, 230, 230);
    COLORREF muted_ = RGB(150, 150, 150);
    COLORREF selected_background_ = RGB(0, 90, 158);
    COLORREF selected_text_ = RGB(255, 255, 255);
    COLORREF header_background_ = RGB(45, 45, 45);

    select_handler on_select_;
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
    const int widths[3] = { column_gesture_, column_command_, column_place_ };

    int left = rc.left;
    for (int column = 0; column < 3; ++column) {
        RECT cell = rc;
        cell.left = left + scale(6);
        cell.right = left + widths[column] - scale(4);
        left += widths[column];

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

    int y = -scroll_;
    if (rows_ != nullptr) {
        for (std::size_t i = 0; i < rows_->size(); ++i) {
            const GestureListRow& row = (*rows_)[i];
            const int height = row_height(row.is_header);
            if (y + height >= 0 && y < client.bottom) {
                RECT row_rect = { 0, y, client.right, y + height };
                draw_row(memory_dc, row_rect, row, static_cast<int>(i) == selected_);
            }
            y += height;
        }
    }

    if (rows_ == nullptr || rows_->empty()) {
        RECT empty = client;
        empty.left += scale(10);
        SetBkMode(memory_dc, TRANSPARENT);
        SetTextColor(memory_dc, muted_);
        DrawTextW(memory_dc, L"Привязок нет - нажмите Reset.", -1, &empty,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

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
        clamp_scroll();
        update_scroll_info();
        return 0;

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
        SetFocus(hwnd_);
        const int row = row_at(GET_Y_LPARAM(lparam));
        if (row >= 0) select_row(row, true);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        scroll_to(scroll_ - (delta / WHEEL_DELTA) * row_height_ * 3);
        return 0;
    }

    case WM_VSCROLL: {
        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int page = client.bottom - client.top;

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
    LRESULT window_proc(UINT msg, WPARAM wparam, LPARAM lparam);

    static void register_window_class();

    void create_children();
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

    //! Шапка таблицы рисуется отдельными Static-контролами: заголовок
    //! ListView в тёмной теме foobar2000 остаётся белым на белом.
    HWND header_gesture_ = nullptr;
    HWND header_command_ = nullptr;
    HWND header_place_ = nullptr;

    //! Строки таблицы: заголовки групп и сами привязки.
    std::vector<GestureListRow> rows_;
    //! Собственный контрол таблицы (см. GestureListControl).
    GestureListControl list_control_;

    HWND count_label_ = nullptr;
    HWND hint_label_ = nullptr;
    HFONT header_font_ = nullptr;

    HWND up_label_ = nullptr;
    HWND down_label_ = nullptr;
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
    HWND dir_backspace_ = nullptr;
    HWND wheel_up_ = nullptr;
    HWND wheel_down_ = nullptr;
    HWND rocker_rl_ = nullptr;
    HWND rocker_lr_ = nullptr;

    HWND gesture_edit_ = nullptr;
    HWND group_combo_ = nullptr;
    HWND command_combo_ = nullptr;
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

    if (settings_.bindings.empty()) {
        // Страховка: страница не должна открываться с пустой таблицей -
        // без строк нельзя ни выбрать привязку, ни понять, что происходит.
        settings_ = GestureConfig::defaults();
        // Хосту сообщим о несохранённых изменениях при первом показе страницы:
        // дёргать callback из конструктора нельзя.
        dirty_ = true;
        pending_state_notify_ = true;
    }
    load_from_settings();

    layout();

    dark_mode_.AddDialogWithControls(hwnd_);
}

GesturePreferencesInstance::~GesturePreferencesInstance() {
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
    common_controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
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
        list_control_.set_columns(scale(160), scale(320), scale(120));
        list_control_.set_select_handler([this](int binding_index) {
            on_list_selection_changed(binding_index);
            update_buttons();
        });
    }

    header_gesture_ = make_static(L"Gesture", SS_LEFT | SS_CENTERIMAGE);
    header_command_ = make_static(L"Command", SS_LEFT | SS_CENTERIMAGE);
    header_place_ = make_static(L"Place", SS_LEFT | SS_CENTERIMAGE);
    count_label_ = make_static(L"", SS_LEFT | SS_CENTERIMAGE);
    hint_label_ = make_static(
        L"1. Соберите жест стрелками / wheel / rocker.\r\n"
        L"2. Выберите команду, нажмите Add New и Apply.\r\n"
        L"Rocker: R+L = держим ПКМ, кликаем ЛКМ; L+R — наоборот.",
        SS_LEFT);

    up_label_ = make_static(L"up", SS_CENTER);
    down_label_ = make_static(L"down", SS_CENTER);
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
    dir_backspace_ = make_button(L"BS", kIdDirBackspace);
    wheel_up_ = make_button(arrow("\xE2\x86\x91").c_str(), kIdWheelUp);
    wheel_down_ = make_button(arrow("\xE2\x86\x93").c_str(), kIdWheelDown);
    rocker_rl_ = make_button(L"R+L", kIdRockerRightLeft);
    rocker_lr_ = make_button(L"L+R", kIdRockerLeftRight);

    gesture_edit_ = make_edit(kIdGestureEdit, ES_READONLY);
    group_combo_ = make_combo(kIdGroupCombo);
    command_combo_ = make_combo(kIdCommandCombo);
    place_combo_ = make_combo(kIdPlaceCombo);

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

    RECT client = {};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    // --- общие метрики (в единицах 96 DPI, масштабируются через scale()) ---
    const int margin = scale(10);
    const int gap = scale(6);
    const int label_height = scale(14);
    const int control_height = scale(24);
    const int header_height = scale(18);

    const int list_top = margin + header_height + scale(2);

    // --- крестовина (D-Pad) -------------------------------------------------
    // Сетка 3x3: [←][BS][→] в одной строке, ↑ над BS, ↓ под BS.
    const int cell = scale(38);
    const int cell_gap = scale(4);

    // Ширины подписей left/right берём по фактическому тексту: фиксированные
    // 44 px сдвигали крестовину вправо и слово "left" не попадало на одну
    // вертикаль с "wheel"/"rocker".
    int side_left = text_width(L"left") + scale(2);
    int side_right = text_width(L"right") + scale(2);
    if (side_left < scale(20)) side_left = scale(20);
    if (side_right < scale(24)) side_right = scale(24);
    const int label_gap = scale(6);

    // Высота крестовины: подпись up + ↑ + ряд + ↓ + подпись down.
    const int pad_height = label_height + control_height + cell_gap + control_height + cell_gap +
                           control_height + label_height;

    // --- wheel / rocker / stroke / галочки ----------------------------------
    const int wheel_height = control_height + label_height;          // кнопки + подписи up/down
    const int rocker_height = control_height;

    // На широкой странице вторая и третья галочки стоят в одном ряду,
    // на узкой - в столбик (тогда рядов три).
    const bool two_columns = (width - 2 * margin) >= scale(720);
    const int checks_rows = two_columns ? 2 : 3;
    const int checks_height = scale(18) * checks_rows + scale(6) * (checks_rows - 1);

    // Левая колонка: крестовина + wheel + rocker + stroke + галочки.
    const int left_content_height = pad_height + scale(10) + wheel_height + scale(8) +
                                    rocker_height + scale(12) + control_height + scale(10) +
                                    checks_height;

    // --- левая колонка: ровно по ширине крестовины --------------------------
    // Подпись "left" начинается ровно на margin - на одной вертикали с
    // "wheel", "rocker" и "Stroke Length". Крестовина идёт сразу за ней,
    // поэтому вся левая колонка получается минимально возможной, а форма -
    // максимально широкой.
    const int pad_x = margin + side_left + label_gap;

    // Правый край крестовины (с подписью "right") задаёт ширину левой колонки.
    const int pad_right = pad_x + 3 * cell + 2 * cell_gap + label_gap + side_right;
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
    const int row_step = control_height + scale(8);
    const int hint_height = label_height * 3 + scale(6);
    const int right_content_height = 4 * row_step + scale(10) +
                                     button_rows * control_height + (button_rows - 1) * gap +
                                     scale(8) + hint_height;

    const int lower_height = left_content_height > right_content_height ? left_content_height
                                                                        : right_content_height;

    // --- подпись "Всего привязок" над крестовиной ---------------------------
    const int count_height = scale(16);
    const int count_gap = scale(6);

    const int count_top = height - margin - lower_height - count_gap - count_height;
    int list_height = count_top - scale(8) - list_top;

    const int min_list_height = scale(80);
    if (list_height < min_list_height) list_height = min_list_height;

    const int list_width = width - 2 * margin;
    MoveWindow(list_, margin, list_top, list_width, list_height, TRUE);

    // Колонки таблицы: жест, команда, место.
    int column_gesture = list_width * 22 / 100;
    int column_command = list_width * 58 / 100;
    int column_place = list_width - column_gesture - column_command;

    const int min_place = scale(80);
    const int min_command = scale(160);
    if (column_place < min_place) {
        column_place = min_place;
        column_command = list_width - column_gesture - column_place;
    }
    if (column_command < min_command && list_width > min_command + min_place + scale(60)) {
        column_command = min_command;
        column_gesture = list_width - column_command - column_place;
    }
    if (column_gesture < scale(60)) column_gesture = scale(60);

    list_control_.set_columns(column_gesture, column_command, column_place);
    list_control_.refresh();

    // Заголовки колонок (Static-контролы над таблицей).
    const int header_x = margin + scale(6);
    MoveWindow(header_gesture_, header_x, margin, column_gesture, header_height, TRUE);
    MoveWindow(header_command_, header_x + column_gesture, margin, column_command, header_height, TRUE);
    MoveWindow(header_place_, header_x + column_gesture + column_command, margin,
               width - margin - (header_x + column_gesture + column_command), header_height, TRUE);

    // Подпись "Всего привязок" - отдельный Static, над крестовиной.
    MoveWindow(count_label_, margin + scale(6), count_top, list_width, count_height, TRUE);

    const int lower_top = count_top + count_height + count_gap;

    // --- левая колонка ------------------------------------------------------
    const int pad_center = pad_x + cell + cell_gap + cell / 2;   // центр колонки BS
    const int up_x = pad_center - cell / 2;
    const int left_x = pad_x;
    const int bs_x = pad_x + cell + cell_gap;
    const int right_x = pad_x + 2 * (cell + cell_gap);
    const int down_x = up_x;

    int y = lower_top;
    MoveWindow(up_label_, up_x, y, cell, label_height, TRUE);
    y += label_height;
    MoveWindow(dir_up_, up_x, y, cell, control_height, TRUE);
    y += control_height + cell_gap;

    MoveWindow(left_label_, margin, y, side_left, control_height, TRUE);
    MoveWindow(dir_left_, left_x, y, cell, control_height, TRUE);
    MoveWindow(dir_backspace_, bs_x, y, cell, control_height, TRUE);
    MoveWindow(dir_right_, right_x, y, cell, control_height, TRUE);
    MoveWindow(right_label_, right_x + cell + label_gap, y, side_right, control_height, TRUE);
    y += control_height + cell_gap;

    MoveWindow(dir_down_, down_x, y, cell, control_height, TRUE);
    y += control_height;
    MoveWindow(down_label_, down_x, y, cell, label_height, TRUE);
    y += label_height + scale(10);

    // --- wheel: подпись слева, кнопки в один ряд, подписи up/down под ними --
    MoveWindow(wheel_label_, margin, y, scale(52), control_height, TRUE);
    const int wheel_x = margin + scale(56);
    MoveWindow(wheel_up_, wheel_x, y, scale(28), control_height, TRUE);
    MoveWindow(wheel_down_, wheel_x + scale(32), y, scale(28), control_height, TRUE);
    MoveWindow(wheel_up_label_, wheel_x, y + control_height, scale(28), label_height, TRUE);
    MoveWindow(wheel_down_label_, wheel_x + scale(32), y + control_height, scale(28), label_height, TRUE);
    y += wheel_height + scale(8);

    // --- rocker: подпись слева, кнопки в один ряд ---------------------------
    MoveWindow(rocker_label_, margin, y, scale(52), control_height, TRUE);
    const int rocker_x = margin + scale(56);
    MoveWindow(rocker_rl_, rocker_x, y, scale(48), control_height, TRUE);
    MoveWindow(rocker_lr_, rocker_x + scale(52), y, scale(48), control_height, TRUE);
    y += rocker_height + scale(12);

    // --- длина штриха -------------------------------------------------------
    MoveWindow(stroke_label_, margin, y, scale(96), control_height, TRUE);
    MoveWindow(stroke_edit_, margin + scale(100), y, scale(56), control_height - scale(2), TRUE);
    MoveWindow(stroke_spin_, margin + scale(157), y, scale(16), control_height - scale(2), TRUE);
    y += control_height + scale(10);

    // --- галочки ------------------------------------------------------------
    const int checks_width = width - 2 * margin;

    MoveWindow(disable_check_, margin, y, scale(280), scale(18), TRUE);
    MoveWindow(alphabet_check_, margin, y + scale(24), scale(300), scale(18), TRUE);

    if (two_columns) {
        MoveWindow(first_only_check_, margin + scale(320), y + scale(24), scale(300), scale(18), TRUE);
        y += scale(18) + scale(24);
    } else {
        MoveWindow(first_only_check_, margin, y + scale(48), scale(300), scale(18), TRUE);
        y += scale(18) + scale(48);
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
    const int buttons_y = lower_top + 4 * row_step + scale(10);

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
        MoveWindow(hint_label_, form_x, buttons_bottom + scale(8), form_width,
                   hint_height, TRUE);
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

    rows_.clear();

    const wchar_t* previous_group_title = nullptr;

    for (std::size_t i = 0; i < settings_.bindings.size(); ++i) {
        const GestureBinding& binding = settings_.bindings[i];

        // Категория жеста: так же, как группы ListView в эталоне.
        const wchar_t* group_title = L"Gesture";
        switch (categorize_gesture(binding.gesture)) {
        case GestureCategory::wheel_up: group_title = L"Wheel Up"; break;
        case GestureCategory::wheel_down: group_title = L"Wheel Down"; break;
        case GestureCategory::rocker: group_title = L"Rocker"; break;
        default: group_title = L"Gesture"; break;
        }

        // Заголовок группы - отдельная (не выбираемая) строка таблицы.
        if (previous_group_title == nullptr || wcscmp(previous_group_title, group_title) != 0) {
            GestureListRow header;
            header.is_header = true;
            header.header = group_title;
            rows_.push_back(std::move(header));
            previous_group_title = group_title;
        }

        GestureListRow row;
        row.binding_index = static_cast<int>(i);
        row.gesture = wide_from_utf8(format_gesture(binding.gesture, settings_.alphabet));
        row.command = wide_from_utf8(std::string(group_label(binding.group)) + ": " + binding.command_path);
        row.place = wide_from_utf8(place_label(binding.place));
        rows_.push_back(std::move(row));
    }

    list_control_.set_rows(&rows_);
    list_control_.refresh();

    if (previous >= 0 && previous < static_cast<int>(settings_.bindings.size())) {
        select_binding(previous);
    }
    updating_ = was_updating;

    if (count_label_ != nullptr) {
        const std::wstring text = L"Всего привязок: " + std::to_wstring(settings_.bindings.size());
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

    ComboBox_ResetContent(command_combo_);
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

    update_buttons();
}

void GesturePreferencesInstance::update_buttons() {
    if (list_ == nullptr || add_button_ == nullptr) return;

    const int selected = selected_row();
    const int count = static_cast<int>(settings_.bindings.size());
    const bool has_selection = selected >= 0 && selected < count;

    const bool has_command = command_combo_ != nullptr && !catalog_.empty() &&
                             ComboBox_GetCurSel(command_combo_) >= 0;

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

    int command_index = -1;
    for (std::size_t i = 0; i < catalog_.size(); ++i) {
        if (catalog_[i].path == binding.command_path) {
            command_index = static_cast<int>(i);
            break;
        }
    }
    if (command_index < 0) {
        // Команда больше не существует (или меню изменилось) - показываем
        // сохранённый путь, чтобы привязку можно было поправить вручную.
        CatalogEntry missing;
        missing.path = binding.command_path;
        missing.group = binding.group;
        catalog_.push_back(missing);

        const std::wstring label = wide_from_utf8(missing.path);
        ComboBox_AddString(command_combo_, label.c_str());
        command_index = static_cast<int>(catalog_.size()) - 1;
    }
    ComboBox_SetCurSel(command_combo_, command_index);

    int place_index = 0;
    const auto& places = command_places();
    for (std::size_t i = 0; i < places.size(); ++i) {
        if (binding.place == places[i].first) place_index = static_cast<int>(i);
    }
    ComboBox_SetCurSel(place_combo_, place_index);

    updating_ = false;
}

void GesturePreferencesInstance::on_add() {
    const std::string gesture = editor_gesture();

    FB2K_console_formatter() << "Mouse Gesture Host: Add New pressed, gesture=\"" << gesture.c_str()
                             << "\", commands=" << static_cast<t_size>(catalog_.size());

    if (gesture.empty()) {
        MessageBoxW(hwnd_,
                    L"Сначала соберите жест: нажмите кнопки-стрелки, "
                    L"wheel или rocker в левой части страницы.",
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
                    L"Не выбрана команда.\n\n"
                    L"Для групп context(...) выделите треки в плейлисте (список команд "
                    L"строится по текущему выделению) либо выберите группу \"main menu\".",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    GestureBinding binding;
    binding.gesture = gesture;
    binding.group = current_group_id();
    binding.command_path = catalog_[static_cast<std::size_t>(command_index)].path;
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
        MessageBoxW(hwnd_, L"Сначала соберите жест в левой части страницы.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int command_index = ComboBox_GetCurSel(command_combo_);
    if (catalog_.empty() || command_index < 0 || command_index >= static_cast<int>(catalog_.size())) {
        MessageBoxW(hwnd_, L"Не выбрана команда - обновлять нечего.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    GestureBinding binding;
    binding.gesture = gesture;
    binding.group = current_group_id();
    binding.command_path = catalog_[static_cast<std::size_t>(command_index)].path;
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
    if (binding_index < 0) return -1;

    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].binding_index == binding_index) return static_cast<int>(i);
    }
    return -1;
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

        switch (id) {
        case kIdDirUp: if (code == BN_CLICKED) on_direction(Token::up); return 0;
        case kIdDirDown: if (code == BN_CLICKED) on_direction(Token::down); return 0;
        case kIdDirLeft: if (code == BN_CLICKED) on_direction(Token::left); return 0;
        case kIdDirRight: if (code == BN_CLICKED) on_direction(Token::right); return 0;
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
