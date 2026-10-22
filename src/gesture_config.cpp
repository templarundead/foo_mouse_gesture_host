#include "gesture_config.h"

namespace mgh {
namespace {

// GUID'ы переменных конфигурации. Никогда не меняйте их - иначе
// пользовательские настройки будут потеряны при обновлении компонента.
constexpr GUID guid_cfg_stroke_length = { 0x3f2b6c1a, 0x9d4e, 0x4b7a, { 0x9c, 0x21, 0x5e, 0x77, 0x8a, 0x10, 0x4d, 0x01 } };
constexpr GUID guid_cfg_flags         = { 0x8a51d3c2, 0x7e6b, 0x4c19, { 0xb2, 0x33, 0x6a, 0x0f, 0x5d, 0x91, 0xc4, 0x02 } };
constexpr GUID guid_cfg_bindings      = { 0x1d7e4f60, 0x2b8a, 0x4d3e, { 0xa5, 0x66, 0x1c, 0x93, 0x7e, 0x20, 0x8f, 0x03 } };

cfg_int g_cfg_stroke_length(guid_cfg_stroke_length, 10);
cfg_int g_cfg_flags(guid_cfg_flags, 0);
cfg_string g_cfg_bindings(guid_cfg_bindings, "");

enum : int {
    flag_disabled = 1 << 0,
    flag_alphabet = 1 << 1,
    flag_first_only = 1 << 2,
    //! Настройки уже применялись; иначе при первом запуске подставляем стандартные привязки.
    flag_initialized = 1 << 3,
};

std::string escape_field(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string unescape_field(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            switch (text[i + 1]) {
            case 't': out.push_back('\t'); ++i; continue;
            case 'n': out.push_back('\n'); ++i; continue;
            case 'r': out.push_back('\r'); ++i; continue;
            case '\\': out.push_back('\\'); ++i; continue;
            default: break;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

void split_fields(std::string_view line, std::vector<std::string>& fields) {
    fields.clear();
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == '\t') {
            fields.push_back(unescape_field(line.substr(begin, i - begin)));
            begin = i + 1;
        }
    }
}

bool g_cfg_bindings_is_empty() {
#if FOOBAR2000_TARGET_VERSION < 81
    const char* stored = g_cfg_bindings.get_ptr();
    return stored == nullptr || stored[0] == '\0';
#else
    return g_cfg_bindings.get().is_empty();
#endif
}

} // namespace

GestureConfig& GestureConfig::instance() {
    static GestureConfig config;
    return config;
}

GestureConfig::GestureConfig() {
    cached_ = load_from_storage();
}

GestureSettings GestureConfig::load_from_storage() const {
    GestureSettings settings;
    settings.stroke_length = clamp_int(static_cast<int>(g_cfg_stroke_length.get_value()), 1, 100);

    const int flags = static_cast<int>(g_cfg_flags.get_value());
    settings.disabled = (flags & flag_disabled) != 0;
    settings.alphabet = (flags & flag_alphabet) != 0;
    settings.first_only = (flags & flag_first_only) != 0;

    // SDK по умолчанию собирает компоненты с FOOBAR2000_TARGET_VERSION=80,
    // где cfg_string - это cfg_var_legacy::cfg_string (наследник pfc::string8).
    // При переходе на target 81+ используется cfg_var_modern::cfg_string с get().
#if FOOBAR2000_TARGET_VERSION < 81
    const char* stored = g_cfg_bindings.get_ptr();
    settings.bindings = deserialize(stored != nullptr ? std::string_view(stored) : std::string_view());
#else
    const pfc::string8 stored = g_cfg_bindings.get();
    settings.bindings = deserialize(std::string_view(stored.c_str(), stored.length()));
#endif

    // Пустая или нечитаемая таблица привязок - это всегда "ещё не настроено":
    // показываем стандартные привязки, чтобы компонент работал сразу после
    // установки, а страница настроек не выглядела сломанной. Полностью
    // отключить жесты можно галочкой "Disable mouse gesture".
    if (settings.bindings.empty()) {
        settings.bindings = defaults().bindings;

        if (!g_cfg_bindings_is_empty()) {
            // В хранилище что-то есть, но разобрать не удалось - покажем это
            // в консоли, чтобы причина не потерялась.
            FB2K_console_formatter()
                << "Mouse Gesture Host: stored gesture table could not be parsed, defaults restored";
        }
    }

    return settings;
}

void GestureConfig::apply(const GestureSettings& settings) {
    GestureSettings normalized = settings;
    normalized.stroke_length = clamp_int(settings.stroke_length, 1, 100);

    int flags = 0;
    if (normalized.disabled) flags |= flag_disabled;
    if (normalized.alphabet) flags |= flag_alphabet;
    if (normalized.first_only) flags |= flag_first_only;
    flags |= flag_initialized;

    g_cfg_stroke_length = normalized.stroke_length;
    g_cfg_flags = flags;

    const std::string serialized = serialize(normalized.bindings);
    g_cfg_bindings = serialized.c_str();

    cached_ = std::move(normalized);
}

GestureSettings GestureConfig::defaults() {
    GestureSettings settings;
    settings.stroke_length = 10;
    settings.bindings = {
        { "L", "main", "Playback/Previous", "main_window" },
        { "R", "main", "Playback/Next", "main_window" },
        { "U", "main", "Playback/Volume up", "main_window" },
        { "D", "main", "Playback/Volume down", "main_window" },
    };
    return settings;
}

std::string GestureConfig::serialize(const std::vector<GestureBinding>& bindings) {
    std::string out;
    for (const auto& binding : bindings) {
        if (binding.gesture.empty()) continue;
        out += escape_field(binding.gesture);
        out.push_back('\t');
        out += escape_field(binding.group);
        out.push_back('\t');
        out += escape_field(binding.command_path);
        out.push_back('\t');
        out += escape_field(binding.place);
        out.push_back('\n');
    }
    return out;
}

std::vector<GestureBinding> GestureConfig::deserialize(std::string_view text) {
    std::vector<GestureBinding> bindings;
    std::vector<std::string> fields;

    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();

        const std::string_view line = text.substr(begin, end - begin);
        begin = end + 1;

        if (line.empty()) continue;

        split_fields(line, fields);
        if (fields.size() < 4) continue;
        if (fields[0].empty() || fields[2].empty()) continue;

        GestureBinding binding;
        binding.gesture = normalize_gesture(fields[0]);
        if (binding.gesture.empty()) continue;
        binding.group = fields[1];
        binding.command_path = fields[2];
        binding.place = fields[3];
        bindings.push_back(std::move(binding));
    }
    return bindings;
}

const std::vector<std::pair<const char*, const char*>>& command_groups() {
    static const std::vector<std::pair<const char*, const char*>> table = {
        { "main",                 "main menu" },
        { "context_ui",           "context(ui selection)" },
        { "context_playlist",     "context(playlist)" },
        { "context_now_playing",  "context(now playing)" },
    };
    return table;
}

const std::vector<std::pair<const char*, const char*>>& command_places() {
    static const std::vector<std::pair<const char*, const char*>> table = {
        { "main_window", "Main Window" },
        { "playlist",    "Playlist" },
        { "now_playing", "Now Playing" },
    };
    return table;
}

const char* group_label(std::string_view group_id) {
    for (const auto& entry : command_groups()) {
        if (group_id == entry.first) return entry.second;
    }
    return "main menu";
}

std::string group_id_from_label(std::string_view label) {
    for (const auto& entry : command_groups()) {
        if (iequals_ascii(label, entry.second) || iequals_ascii(label, entry.first)) return entry.first;
    }
    return "main";
}

const char* place_label(std::string_view place_id) {
    for (const auto& entry : command_places()) {
        if (place_id == entry.first) return entry.second;
    }
    return "Main Window";
}

std::string place_id_from_label(std::string_view label) {
    for (const auto& entry : command_places()) {
        if (iequals_ascii(label, entry.second) || iequals_ascii(label, entry.first)) return entry.first;
    }
    return "main_window";
}

} // namespace mgh
