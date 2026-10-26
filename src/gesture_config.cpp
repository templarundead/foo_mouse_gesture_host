#include "common.h"
#include "gesture_config.h"

namespace mgh {
namespace {

// GUID'ы переменных конфигурации. Существующие GUID никогда не меняются.
constexpr GUID guid_cfg_stroke_length = { 0x3f2b6c1a, 0x9d4e, 0x4b7a, { 0x9c, 0x21, 0x5e, 0x77, 0x8a, 0x10, 0x4d, 0x01 } };
constexpr GUID guid_cfg_flags         = { 0x8a51d3c2, 0x7e6b, 0x4c19, { 0xb2, 0x33, 0x6a, 0x0f, 0x5d, 0x91, 0xc4, 0x02 } };
constexpr GUID guid_cfg_bindings      = { 0x1d7e4f60, 0x2b8a, 0x4d3e, { 0xa5, 0x66, 0x1c, 0x93, 0x7e, 0x20, 0x8f, 0x03 } };
// Новый отдельный ключ: последняя валидная таблица для восстановления после corruption.
constexpr GUID guid_cfg_bindings_backup = { 0x71a6131f, 0xae52, 0x496c, { 0x80, 0xcc, 0x84, 0x4f, 0x7a, 0xd0, 0x1d, 0x6a } };

cfg_int g_cfg_stroke_length(guid_cfg_stroke_length, 8);
cfg_int g_cfg_flags(guid_cfg_flags, 0);
cfg_string g_cfg_bindings(guid_cfg_bindings, "");
cfg_string g_cfg_bindings_backup(guid_cfg_bindings_backup, "");

enum : int {
    flag_disabled = 1 << 0,
    flag_alphabet = 1 << 1,
    flag_first_only = 1 << 2,
    //! Настройки уже применялись; иначе первый пустой запуск получает defaults.
    flag_initialized = 1 << 3,
};

std::string read_cfg_string(cfg_string& value) {
#if FOOBAR2000_TARGET_VERSION < 81
    const char* stored = value.get_ptr();
    return stored != nullptr ? std::string(stored) : std::string();
#else
    const pfc::string8 stored = value.get();
    return std::string(stored.c_str(), stored.length());
#endif
}

void log_diagnostics(std::string_view source, const BindingParseResult& parsed) {
    constexpr std::size_t kMaxDetailedMessages = 12;
    for (std::size_t i = 0; i < parsed.diagnostics.size() && i < kMaxDetailedMessages; ++i) {
        const auto& diagnostic = parsed.diagnostics[i];
        FB2K_console_formatter() << "Mouse Gesture Host: " << source.data()
                                 << " row " << static_cast<t_size>(diagnostic.line)
                                 << ": " << diagnostic.message.c_str();
    }
    if (parsed.diagnostics.size() > kMaxDetailedMessages) {
        FB2K_console_formatter() << "Mouse Gesture Host: " << source.data() << ", "
                                 << static_cast<t_size>(parsed.diagnostics.size() - kMaxDetailedMessages)
                                 << " more diagnostic(s) suppressed";
    }
    if (parsed.dropped_rows != 0 || parsed.duplicate_rows != 0) {
        FB2K_console_formatter() << "Mouse Gesture Host: " << source.data() << " summary: "
                                 << static_cast<t_size>(parsed.dropped_rows) << " invalid row(s), "
                                 << static_cast<t_size>(parsed.duplicate_rows) << " exact duplicate(s)";
    }
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
    const bool initialized = (flags & flag_initialized) != 0;
    settings.disabled = (flags & flag_disabled) != 0;
    settings.alphabet = (flags & flag_alphabet) != 0;
    settings.first_only = (flags & flag_first_only) != 0;

    const std::string stored = read_cfg_string(g_cfg_bindings);
    BindingParseResult primary = deserialize(stored);
    log_diagnostics("binding table", primary);

    if (primary.valid) {
        settings.bindings = std::move(primary.bindings);
        // Только самый первый запуск без флага получает defaults. Пустая таблица
        // после Apply является осознанным выбором и не восстанавливается автоматически.
        if (stored.empty() && !initialized) {
            settings.bindings = defaults().bindings;
        } else if (primary.schema == BindingSchema::legacy_v1) {
            FB2K_console_formatter() << "Mouse Gesture Host: legacy binding table loaded; "
                                        "it will be written as schema v2 on the next Apply";
        }
        return settings;
    }

    FB2K_console_formatter() << "Mouse Gesture Host: stored binding table is corrupt; trying the last valid backup";
    const std::string backup = read_cfg_string(g_cfg_bindings_backup);
    if (!backup.empty()) {
        BindingParseResult restored = deserialize(backup);
        if (restored.valid) {
            settings.bindings = std::move(restored.bindings);
            FB2K_console_formatter() << "Mouse Gesture Host: restored "
                                     << static_cast<t_size>(settings.bindings.size())
                                     << " binding(s) from the last valid backup";
            return settings;
        }
        log_diagnostics("binding backup", restored);
        FB2K_console_formatter() << "Mouse Gesture Host: binding backup is also invalid";
    } else {
        FB2K_console_formatter() << "Mouse Gesture Host: no binding backup is available";
    }

    // Fail closed: сохраняем только корректно разобранные строки, а при полной
    // corruption получаем пустую таблицу, а не неожиданные defaults.
    settings.bindings = std::move(primary.bindings);
    FB2K_console_formatter() << "Mouse Gesture Host: using "
                             << static_cast<t_size>(settings.bindings.size())
                             << " valid row(s) from the corrupt primary table";
    return settings;
}

void GestureConfig::apply(const GestureSettings& settings) {
    GestureSettings normalized = settings;
    normalized.stroke_length = clamp_int(settings.stroke_length, 1, 100);

    // Применяем те же строгие правила, что и при загрузке. Это также нормализует
    // жесты и GUID, удаляя только полные дубликаты с сохранением порядка строк.
    BindingParseResult checked = deserialize(serialize(normalized.bindings));
    log_diagnostics("applied binding table", checked);
    normalized.bindings = std::move(checked.bindings);
    const std::string serialized = serialize(normalized.bindings);

    int flags = 0;
    if (normalized.disabled) flags |= flag_disabled;
    if (normalized.alphabet) flags |= flag_alphabet;
    if (normalized.first_only) flags |= flag_first_only;
    flags |= flag_initialized;

    const std::string previous = read_cfg_string(g_cfg_bindings);
    BindingParseResult previous_valid = deserialize(previous);
    if (previous_valid.valid && (!previous.empty() || (g_cfg_flags.get_value() & flag_initialized) != 0)) {
        if (previous.empty()) {
            const std::string empty_table = serialize({});
            g_cfg_bindings_backup = empty_table.c_str();
        } else {
            g_cfg_bindings_backup = previous.c_str();
        }
    }

    g_cfg_stroke_length = normalized.stroke_length;
    g_cfg_flags = flags;
    g_cfg_bindings = serialized.c_str();

    cached_ = std::move(normalized);
}

} // namespace mgh
