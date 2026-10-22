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

    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                            0, 0, 200, 200, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)), instance, nullptr);
    if (font_ != nullptr) SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_EnableGroupView(list_, TRUE);
    add_list_column(0, L"Gesture", scale(160));
    add_list_column(1, L"Command", scale(320));
    add_list_column(2, L"Place", scale(120));

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

void GesturePreferencesInstance::layout() {
    if (hwnd_ == nullptr || list_ == nullptr) return;

    RECT client = {};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    const int margin = scale(8);
    const int gap = scale(6);
    const int label_height = scale(14);
    const int control_height = scale(24);

    int list_height = height - scale(300);
    if (list_height < scale(120)) list_height = scale(120);
    if (list_height > height - scale(140)) list_height = height - scale(140);
    if (list_height < scale(80)) list_height = scale(80);

    MoveWindow(list_, margin, margin, width - 2 * margin, list_height, TRUE);

    const int list_width = width - 2 * margin;
    ListView_SetColumnWidth(list_, 0, list_width * 26 / 100);
    ListView_SetColumnWidth(list_, 1, list_width * 54 / 100);
    ListView_SetColumnWidth(list_, 2, list_width * 20 / 100);

    const int top = margin + list_height + scale(12);

    int left_width = width * 45 / 100;
    left_width = clamp_int(left_width, scale(240), scale(360));

    // --- крестовина направлений ---
    const int cell_width = scale(38);
    const int cell_height = control_height;
    const int cell_gap = scale(4);
    const int pad_center = margin + scale(78);

    const int up_y = top + label_height + 1;
    const int mid_y = up_y + cell_height + cell_gap;
    const int down_y = mid_y + cell_height + cell_gap;
    const int down_label_y = down_y + cell_height + 1;

    MoveWindow(up_label_, pad_center - scale(30), top, scale(60), label_height, TRUE);
    MoveWindow(dir_up_, pad_center - cell_width / 2, up_y, cell_width, cell_height, TRUE);

    MoveWindow(dir_left_, pad_center - cell_width / 2 - cell_gap - cell_width, mid_y, cell_width, cell_height, TRUE);
    MoveWindow(dir_backspace_, pad_center - cell_width / 2, mid_y, cell_width, cell_height, TRUE);
    MoveWindow(dir_right_, pad_center - cell_width / 2 + cell_gap + cell_width, mid_y, cell_width, cell_height, TRUE);

    MoveWindow(left_label_, pad_center - cell_width / 2 - cell_gap - cell_width - scale(46),
               mid_y + scale(4), scale(42), label_height, TRUE);
    MoveWindow(right_label_, pad_center + cell_width / 2 + cell_gap + scale(6),
               mid_y + scale(4), scale(42), label_height, TRUE);

    MoveWindow(dir_down_, pad_center - cell_width / 2, down_y, cell_width, cell_height, TRUE);
    MoveWindow(down_label_, pad_center - scale(30), down_label_y, scale(60), label_height, TRUE);

    // --- колесо и рокер ---
    const int wheel_y = down_label_y + label_height + scale(16);

    MoveWindow(wheel_label_, margin, wheel_y + scale(3), scale(44), label_height, TRUE);
    MoveWindow(wheel_up_, margin + scale(48), wheel_y, scale(28), control_height, TRUE);
    MoveWindow(wheel_down_, margin + scale(80), wheel_y, scale(28), control_height, TRUE);
    MoveWindow(wheel_up_label_, margin + scale(48), wheel_y + control_height + 1, scale(28), label_height, TRUE);
    MoveWindow(wheel_down_label_, margin + scale(80), wheel_y + control_height + 1, scale(28), label_height, TRUE);

    const int rocker_y = wheel_y + control_height + label_height + scale(6);
    MoveWindow(rocker_label_, margin, rocker_y + scale(3), scale(44), label_height, TRUE);
    MoveWindow(rocker_rl_, margin + scale(48), rocker_y, scale(48), control_height, TRUE);
    MoveWindow(rocker_lr_, margin + scale(100), rocker_y, scale(48), control_height, TRUE);

    // --- длина штриха ---
    const int stroke_y = rocker_y + control_height + scale(14);
    MoveWindow(stroke_label_, margin, stroke_y + scale(4), scale(92), label_height, TRUE);
    MoveWindow(stroke_edit_, margin + scale(94), stroke_y, scale(56), control_height - scale(2), TRUE);
    MoveWindow(stroke_spin_, margin + scale(151), stroke_y, scale(16), control_height - scale(2), TRUE);

    // --- галочки ---
    const int check_y = stroke_y + control_height + scale(12);
    const int check_width = left_width - margin;
    MoveWindow(disable_check_, margin, check_y, check_width, scale(18), TRUE);
    MoveWindow(alphabet_check_, margin, check_y + scale(24), check_width, scale(18), TRUE);
    if (left_width >= scale(360)) {
        MoveWindow(first_only_check_, margin + scale(210), check_y + scale(24), check_width - scale(200), scale(18), TRUE);
    } else {
        MoveWindow(first_only_check_, margin, check_y + scale(48), check_width, scale(18), TRUE);
    }

    // --- правая колонка с редактором привязки ---
    const int form_x = margin + left_width + scale(16);
    const int form_width = width - margin - form_x;
    const int label_width = scale(80);
    const int editor_x = form_x + label_width;
    const int editor_width = form_width - label_width;
    const int row = control_height + scale(8);

    MoveWindow(gesture_label_, form_x, top + scale(4), label_width, label_height, TRUE);
    MoveWindow(gesture_edit_, editor_x, top, editor_width, control_height, TRUE);

    MoveWindow(group_label_, form_x, top + row + scale(4), label_width, label_height, TRUE);
    SetWindowPos(group_combo_, nullptr, editor_x, top + row, editor_width, scale(200), SWP_NOZORDER | SWP_NOACTIVATE);

    MoveWindow(command_label_, form_x, top + 2 * row + scale(4), label_width, label_height, TRUE);
    SetWindowPos(command_combo_, nullptr, editor_x, top + 2 * row, editor_width, scale(240), SWP_NOZORDER | SWP_NOACTIVATE);

    MoveWindow(place_label_, form_x, top + 3 * row + scale(4), label_width, label_height, TRUE);
    SetWindowPos(place_combo_, nullptr, editor_x, top + 3 * row, editor_width < scale(190) ? editor_width : scale(190),
                 scale(160), SWP_NOZORDER | SWP_NOACTIVATE);

    // --- кнопки управления таблицей ---
    const int buttons_y = top + 4 * row + scale(8);
    const int base_widths[5] = { 90, 66, 66, 54, 54 };
    HWND buttons[5] = { add_button_, update_button_, delete_button_, move_up_button_, move_down_button_ };

    int total_width = 4 * gap;
    for (const int base : base_widths) total_width += scale(base);

    const double factor = form_width < total_width ? static_cast<double>(form_width) / static_cast<double>(total_width) : 1.0;

    int x = form_x;
    for (int i = 0; i < 5; ++i) {
        const int button_width = static_cast<int>(scale(base_widths[i]) * factor);
        MoveWindow(buttons[i], x, buttons_y, button_width, control_height, TRUE);
        x += button_width + gap;
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

        if (group != last_group) {
            const wchar_t* header = L"Gesture";
            switch (group) {
            case kGroupWheelUp: header = L"Wheel Up"; break;
            case kGroupWheelDown: header = L"Wheel Down"; break;
            case kGroupRocker: header = L"Rocker"; break;
            default: header = L"Gesture"; break;
            }

            LVGROUP lvgroup = {};
            lvgroup.cbSize = sizeof(lvgroup);
            lvgroup.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_ALIGN;
            lvgroup.iGroupId = group;
            lvgroup.uAlign = LVGA_HEADER_LEFT;
            lvgroup.pszHeader = const_cast<LPWSTR>(header);
            ListView_InsertGroup(list_, -1, &lvgroup);

            last_group = group;
        }

        const std::wstring gesture_text = wide_from_utf8(format_gesture(binding.gesture, settings_.alphabet));
        const std::wstring command_text =
            wide_from_utf8(std::string(group_label(binding.group)) + ": " + binding.command_path);
        const std::wstring place_text = wide_from_utf8(place_label(binding.place));

        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
        item.iItem = static_cast<int>(i);
        item.iGroupId = group;
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
    update_buttons();
    updating_ = was_updating;
}

void GesturePreferencesInstance::refresh_gesture_text() {
    const std::wstring text = wide_from_utf8(format_gesture(edit_gesture_, settings_.alphabet));
    SetWindowTextW(gesture_edit_, text.c_str());
}

void GesturePreferencesInstance::refresh_command_list() {
    if (command_combo_ == nullptr) return;

    const std::string group_id = current_group_id();
    catalog_ = list_commands(group_id);

    ComboBox_ResetContent(command_combo_);
    for (const auto& entry : catalog_) {
        const std::wstring label = wide_from_utf8(entry.path);
        ComboBox_AddString(command_combo_, label.c_str());
    }
    if (!catalog_.empty()) ComboBox_SetCurSel(command_combo_, 0);
}

void GesturePreferencesInstance::update_buttons() {
    const int selected = selected_row();
    const int count = static_cast<int>(settings_.bindings.size());
    const bool has_selection = selected >= 0 && selected < count;

    EnableWindow(update_button_, has_selection);
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
        MessageBeep(MB_ICONWARNING);
        return;
    }

    const int command_index = ComboBox_GetCurSel(command_combo_);
    if (command_index < 0 || command_index >= static_cast<int>(catalog_.size())) {
        MessageBeep(MB_ICONWARNING);
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
        MessageBeep(MB_ICONWARNING);
        return;
    }

    const int command_index = ComboBox_GetCurSel(command_combo_);
    if (command_index < 0 || command_index >= static_cast<int>(catalog_.size())) {
        MessageBeep(MB_ICONWARNING);
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
