#include "preferences_page.h"

#include "command_catalog.h"
#include "gesture_engine.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cwchar>

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
    void add_list_column(int index, const wchar_t* title, int width);
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
    int selected_row() const;
    int read_stroke_length() const;

    HWND make_button(const wchar_t* text, int id, DWORD extra_style = 0);
    HWND make_static(const wchar_t* text, DWORD style);
    HWND make_edit(int id, DWORD extra_style = 0);
    HWND make_combo(int id);
    HWND make_checkbox(const wchar_t* text, int id);

    int scale(int value) const { return MulDiv(value, dpi_, 96); }

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

    //! Удалось ли включить group view (иначе тип жеста пишется в первую колонку).
    bool groups_enabled_ = false;

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

void GesturePreferencesInstance::add_list_column(int index, const wchar_t* title, int width) {
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.cx = width;
    column.iSubItem = index;
    column.pszText = const_cast<LPWSTR>(title);
    ListView_InsertColumn(list_, index, &column);
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

    // Заголовок ListView не используем (LVS_NOCOLUMNHEADER): в тёмной теме
    // foobar2000 он остаётся белым, а текст на нём не читается. Вместо него
    // над таблицей стоят обычные Static-контролы.
    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | LVS_NOCOLUMNHEADER,
                            0, 0, 200, 120, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)), instance, nullptr);
    if (list_ == nullptr) {
        FB2K_console_formatter() << "Mouse Gesture Host: failed to create the gesture list ("
                                 << static_cast<unsigned long>(GetLastError()) << ")";
    } else {
        if (font_ != nullptr) SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        add_list_column(0, L"Gesture", scale(160));
        add_list_column(1, L"Command", scale(320));
        add_list_column(2, L"Place", scale(120));

        // group view поддерживается не всеми версиями comctl32; если не вышло -
        // тип жеста будет дописан в первую колонку, но строки всё равно видны.
        groups_enabled_ = ListView_EnableGroupView(list_, TRUE) != 0;
    }

    header_gesture_ = make_static(L"Gesture", SS_LEFT | SS_CENTERIMAGE);
    header_command_ = make_static(L"Command", SS_LEFT | SS_CENTERIMAGE);
    header_place_ = make_static(L"Place", SS_LEFT | SS_CENTERIMAGE);

    up_label_ = make_static(L"up", SS_CENTER);
    down_label_ = make_static(L"down", SS_CENTER);
    left_label_ = make_static(L"left", SS_RIGHT);
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

void GesturePreferencesInstance::layout() {
    if (hwnd_ == nullptr || list_ == nullptr) return;

    RECT client = {};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    const int margin = scale(10);
    const int gap = scale(6);
    const int label_height = scale(14);
    const int control_height = scale(24);
    const int pad_cell = scale(38);
    const int pad_gap = scale(4);
    const int header_height = scale(18);
    const int list_top = margin + header_height + scale(2);

    // Вертикальные отступы блока настроек. В "tight" варианте они уменьшены,
    // чтобы содержимое помещалось на невысоких страницах (маленький экран,
    // крупный системный шрифт).
    struct Metrics {
        int gap_wheel;
        int gap_after_wheel;
        int gap_rocker;
        int gap_stroke;
        int check_step;
    };

    const Metrics spacious = { scale(16), scale(8), scale(14), scale(12), scale(24) };
    const Metrics compact  = { scale(10), scale(4), scale(8),  scale(6),  scale(20) };

    const int row_step = control_height + scale(8);

    // Один и тот же код и измеряет блок, и расставляет контролы - так разметка
    // не может "разъехаться" с расчётом высоты.
    auto place_lower = [&](int top, const Metrics& metrics, bool repaint) -> int {
        const BOOL paint = repaint ? TRUE : FALSE;
        int y = top;

        // --- крестовина направлений ---
        const int pad_x = margin;
        const int bs_x = pad_x + 2 * (pad_cell + pad_gap);
        const int pad_center = bs_x + pad_cell / 2;

        MoveWindow(up_label_, pad_center - scale(30), y, scale(60), label_height, paint);
        y += label_height;
        MoveWindow(dir_up_, bs_x, y, pad_cell, control_height, paint);
        y += control_height + pad_gap;

        MoveWindow(left_label_, pad_x, y + scale(4), pad_cell, label_height, paint);
        MoveWindow(dir_left_, pad_x + pad_cell + pad_gap, y, pad_cell, control_height, paint);
        MoveWindow(dir_backspace_, bs_x, y, pad_cell, control_height, paint);
        MoveWindow(dir_right_, bs_x + pad_cell + pad_gap, y, pad_cell, control_height, paint);
        MoveWindow(right_label_, bs_x + 2 * (pad_cell + pad_gap), y + scale(4), pad_cell, label_height, paint);
        y += control_height + pad_gap;

        MoveWindow(dir_down_, bs_x, y, pad_cell, control_height, paint);
        MoveWindow(down_label_, pad_center - scale(30), y + control_height + 1, scale(60), label_height, paint);
        y += control_height + label_height + 1 + metrics.gap_wheel;

        // --- колесо ---
        MoveWindow(wheel_label_, margin, y + scale(3), scale(44), label_height, paint);
        MoveWindow(wheel_up_, margin + scale(48), y, scale(28), control_height, paint);
        MoveWindow(wheel_down_, margin + scale(80), y, scale(28), control_height, paint);
        MoveWindow(wheel_up_label_, margin + scale(48), y + control_height + 1, scale(28), label_height, paint);
        MoveWindow(wheel_down_label_, margin + scale(80), y + control_height + 1, scale(28), label_height, paint);
        y += control_height + label_height + 1 + metrics.gap_after_wheel;

        // --- рокер ---
        MoveWindow(rocker_label_, margin, y + scale(3), scale(44), label_height, paint);
        MoveWindow(rocker_rl_, margin + scale(48), y, scale(48), control_height, paint);
        MoveWindow(rocker_lr_, margin + scale(100), y, scale(48), control_height, paint);
        y += control_height + metrics.gap_rocker;

        // --- длина штриха ---
        MoveWindow(stroke_label_, margin, y + scale(3), scale(96), label_height, paint);
        MoveWindow(stroke_edit_, margin + scale(98), y, scale(56), control_height - scale(2), paint);
        MoveWindow(stroke_spin_, margin + scale(155), y, scale(16), control_height - scale(2), paint);
        y += control_height + metrics.gap_stroke;

        // --- галочки (в столбик: на узкой странице они больше не наезжают друг на друга) ---
        MoveWindow(disable_check_, margin, y, scale(260), scale(18), paint);
        MoveWindow(alphabet_check_, margin, y + metrics.check_step, scale(300), scale(18), paint);
        MoveWindow(first_only_check_, margin, y + 2 * metrics.check_step, scale(300), scale(18), paint);
        y += scale(18) + 2 * metrics.check_step;

        // --- правая колонка: редактор привязки ---
        int left_width = width * 42 / 100;
        left_width = clamp_int(left_width, scale(250), scale(380));

        const int form_x = margin + left_width + scale(16);
        const int form_width = width - margin - form_x;

        int label_width = scale(84);
        int editor_width = form_width - label_width;
        if (editor_width < scale(140)) {
            label_width = scale(60);
            editor_width = form_width - label_width;
        }
        if (editor_width < scale(80)) editor_width = scale(80);
        const int editor_x = form_x + label_width;

        MoveWindow(gesture_label_, form_x, top + scale(4), label_width, label_height, paint);
        MoveWindow(gesture_edit_, editor_x, top, editor_width, control_height, paint);

        MoveWindow(group_label_, form_x, top + row_step + scale(4), label_width, label_height, paint);
        SetWindowPos(group_combo_, nullptr, editor_x, top + row_step, editor_width, scale(240),
                     SWP_NOZORDER | SWP_NOACTIVATE | (repaint ? 0 : SWP_NOREDRAW));

        MoveWindow(command_label_, form_x, top + 2 * row_step + scale(4), label_width, label_height, paint);
        SetWindowPos(command_combo_, nullptr, editor_x, top + 2 * row_step, editor_width, scale(240),
                     SWP_NOZORDER | SWP_NOACTIVATE | (repaint ? 0 : SWP_NOREDRAW));

        MoveWindow(place_label_, form_x, top + 3 * row_step + scale(4), label_width, label_height, paint);
        SetWindowPos(place_combo_, nullptr, editor_x, top + 3 * row_step,
                     editor_width < scale(190) ? editor_width : scale(190), scale(160),
                     SWP_NOZORDER | SWP_NOACTIVATE | (repaint ? 0 : SWP_NOREDRAW));

        // --- кнопки управления таблицей ---
        const int buttons_y = top + 4 * row_step + scale(10);
        const int base_widths[5] = { 90, 66, 66, 54, 54 };
        HWND buttons[5] = { add_button_, update_button_, delete_button_, move_up_button_, move_down_button_ };

        int total_width = 4 * gap;
        for (const int base : base_widths) total_width += scale(base);

        const double factor = form_width < total_width
            ? static_cast<double>(form_width) / static_cast<double>(total_width)
            : 1.0;

        int x = form_x;
        for (int i = 0; i < 5; ++i) {
            const int button_width = static_cast<int>(scale(base_widths[i]) * factor);
            MoveWindow(buttons[i], x, buttons_y, button_width, control_height, paint);
            x += button_width + gap;
        }

        const int right_bottom = buttons_y + control_height;
        return y > right_bottom ? y : right_bottom;
    };

    // Измеряем блок сначала с обычными отступами; если не помещается - сжимаем.
    const int min_list_height = scale(90);

    const Metrics* metrics = &spacious;
    int lower_bottom = place_lower(0, *metrics, false);
    int top = height - margin - lower_bottom;

    if (top - scale(12) - list_top < min_list_height) {
        metrics = &compact;
        lower_bottom = place_lower(0, *metrics, false);
        top = height - margin - lower_bottom;
        if (top - scale(12) - list_top < min_list_height) {
            // Совсем мало места: список ужимается до минимума.
            top = list_top + scale(12) + min_list_height;
        }
    }

    const int list_height = top - scale(12) - list_top;

    // --- таблица привязок ---
    const int list_width = width - 2 * margin;
    MoveWindow(list_, margin, list_top, list_width, list_height, TRUE);

    int column_gesture = list_width * 26 / 100;
    int column_command = list_width * 54 / 100;
    int column_place = list_width - column_gesture - column_command;

    const int min_place = scale(70);
    if (column_place < min_place) {
        column_place = min_place;
        column_command = list_width - column_gesture - column_place;
        if (column_command < scale(120)) {
            column_command = scale(120);
            column_gesture = list_width - column_command - column_place;
            if (column_gesture < scale(70)) column_gesture = scale(70);
        }
    }

    ListView_SetColumnWidth(list_, 0, column_gesture);
    ListView_SetColumnWidth(list_, 1, column_command);
    ListView_SetColumnWidth(list_, 2, column_place);

    // Заголовки таблицы - обычные Static-контролы (заголовок ListView
    // в тёмной теме остаётся белым на белом).
    const int header_x = margin + scale(2);
    MoveWindow(header_gesture_, header_x, margin, column_gesture, header_height, TRUE);
    MoveWindow(header_command_, header_x + column_gesture, margin, column_command, header_height, TRUE);
    MoveWindow(header_place_, header_x + column_gesture + column_command, margin,
               width - margin - (header_x + column_gesture + column_command), header_height, TRUE);

    // Финальная расстановка нижнего блока на вычисленном месте.
    place_lower(top, *metrics, true);
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

    ListView_SetBkColor(list_, list_background);
    ListView_SetTextBkColor(list_, list_background);
    ListView_SetTextColor(list_, dark ? RGB(230, 230, 230) : GetSysColor(COLOR_WINDOWTEXT));
    SetWindowTheme(list_, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    InvalidateRect(list_, nullptr, TRUE);
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
    ListView_DeleteAllItems(list_);
    ListView_RemoveAllGroups(list_);

    int last_group = 0;
    for (std::size_t i = 0; i < settings_.bindings.size(); ++i) {
        const GestureBinding& binding = settings_.bindings[i];

        int group = kGroupGesture;
        switch (categorize_gesture(binding.gesture)) {
        case GestureCategory::wheel_up: group = kGroupWheelUp; break;
        case GestureCategory::wheel_down: group = kGroupWheelDown; break;
        case GestureCategory::rocker: group = kGroupRocker; break;
        default: group = kGroupGesture; break;
        }

        const wchar_t* group_title = L"Gesture";
        switch (group) {
        case kGroupWheelUp: group_title = L"Wheel Up"; break;
        case kGroupWheelDown: group_title = L"Wheel Down"; break;
        case kGroupRocker: group_title = L"Rocker"; break;
        default: group_title = L"Gesture"; break;
        }

        if (groups_enabled_ && group != last_group) {
            LVGROUP lvgroup = {};
            lvgroup.cbSize = sizeof(lvgroup);
            lvgroup.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_ALIGN;
            lvgroup.iGroupId = group;
            lvgroup.uAlign = LVGA_HEADER_LEFT;
            lvgroup.pszHeader = const_cast<LPWSTR>(group_title);
            if (ListView_InsertGroup(list_, -1, &lvgroup) < 0) {
                // Группы не поддерживаются - дальше показываем плоский список.
                groups_enabled_ = false;
            }

            last_group = group;
        }

        std::wstring gesture_text = wide_from_utf8(format_gesture(binding.gesture, settings_.alphabet));
        if (!groups_enabled_) {
            // Плоский режим: тип жеста виден прямо в первой колонке.
            if (group != kGroupGesture) {
                gesture_text = std::wstring(group_title) + L": " + gesture_text;
            }
        }

        const std::wstring command_text =
            wide_from_utf8(std::string(group_label(binding.group)) + ": " + binding.command_path);
        const std::wstring place_text = wide_from_utf8(place_label(binding.place));

        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        if (groups_enabled_) {
            item.mask |= LVIF_GROUPID;
            item.iGroupId = group;
        }
        item.lParam = static_cast<LPARAM>(i);
        item.pszText = const_cast<LPWSTR>(gesture_text.c_str());

        const int inserted = ListView_InsertItem(list_, &item);
        if (inserted >= 0) {
            ListView_SetItemText(list_, inserted, 1, const_cast<LPWSTR>(command_text.c_str()));
            ListView_SetItemText(list_, inserted, 2, const_cast<LPWSTR>(place_text.c_str()));
        }
    }

    if (previous >= 0 && previous < static_cast<int>(settings_.bindings.size())) {
        ListView_SetItemState(list_, previous, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    updating_ = was_updating;

    // Страховка: если group view включён, но строки не отрисовались
    // (встречается на некоторых сборках comctl32), перерисовываем плоским списком.
    if (groups_enabled_ && !settings_.bindings.empty() && ListView_GetItemCount(list_) == 0) {
        groups_enabled_ = false;
        ListView_EnableGroupView(list_, FALSE);
        ListView_RemoveAllGroups(list_);
        refresh_list();
        return;
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
    const int selected = selected_row();
    const int count = static_cast<int>(settings_.bindings.size());
    const bool has_selection = selected >= 0 && selected < count;

    const bool has_command = !catalog_.empty() && ComboBox_GetCurSel(command_combo_) >= 0;

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

void GesturePreferencesInstance::on_list_selection_changed(int index) {
    if (updating_) return;
    if (index < 0 || index >= static_cast<int>(settings_.bindings.size())) return;

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
    if (gesture.empty()) {
        MessageBoxW(hwnd_,
                    L"Сначала соберите жест: нажмите кнопки-стрелки, "
                    L"wheel или rocker в левой части страницы.",
                    L"Mouse Gesture Host", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int command_index = ComboBox_GetCurSel(command_combo_);
    if (catalog_.empty() || command_index < 0 || command_index >= static_cast<int>(catalog_.size())) {
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
    ListView_SetItemState(list_, last, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list_, last, FALSE);

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
    ListView_SetItemState(list_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

    set_dirty(true);
}

void GesturePreferencesInstance::on_delete() {
    const int index = selected_row();
    if (index < 0 || index >= static_cast<int>(settings_.bindings.size())) return;

    settings_.bindings.erase(settings_.bindings.begin() + index);
    refresh_list();

    const int new_index = index < static_cast<int>(settings_.bindings.size()) ? index
                                                                              : static_cast<int>(settings_.bindings.size()) - 1;
    if (new_index >= 0) {
        ListView_SetItemState(list_, new_index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, new_index, FALSE);
    }
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
    ListView_SetItemState(list_, target, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list_, target, FALSE);
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

int GesturePreferencesInstance::selected_row() const {
    if (list_ == nullptr) return -1;
    return ListView_GetNextItem(list_, -1, LVNI_SELECTED);
}

int GesturePreferencesInstance::read_stroke_length() const {
    const std::wstring text = read_window_text(stroke_edit_);
    if (text.empty()) return 10;

    wchar_t* end = nullptr;
    const long value = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str()) return 10;
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

    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lparam);
        if (header != nullptr && header->idFrom == kIdList && header->code == LVN_ITEMCHANGED) {
            auto* list_view = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((list_view->uNewState & LVIS_SELECTED) != 0 &&
                (list_view->uOldState & LVIS_SELECTED) == 0) {
                on_list_selection_changed(list_view->iItem);
                update_buttons();
            }
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
