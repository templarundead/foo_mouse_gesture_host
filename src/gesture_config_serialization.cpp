#include "gesture_config.h"

#include <unordered_set>

namespace mgh {
namespace {

constexpr std::string_view kSchemaV2Header = "MGH2";

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

bool unescape_field(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\0' || c == '\r') return false;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= text.size()) return false;
        const char escaped = text[++i];
        switch (escaped) {
        case 't': out.push_back('\t'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case '\\': out.push_back('\\'); break;
        default: return false;
        }
    }
    return true;
}

bool split_fields(std::string_view line, std::vector<std::string>& fields) {
    fields.clear();
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i != line.size() && line[i] != '\t') continue;
        std::string field;
        if (!unescape_field(line.substr(begin, i - begin), field)) return false;
        fields.push_back(std::move(field));
        begin = i + 1;
    }
    return true;
}

bool is_valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first == 0) return false;
        if (first < 0x80) {
            ++i;
            continue;
        }

        std::uint32_t codepoint = 0;
        std::size_t length = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xE0) == 0xC0) {
            codepoint = first & 0x1F;
            length = 2;
            minimum = 0x80;
        } else if ((first & 0xF0) == 0xE0) {
            codepoint = first & 0x0F;
            length = 3;
            minimum = 0x800;
        } else if ((first & 0xF8) == 0xF0) {
            codepoint = first & 0x07;
            length = 4;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool normalize_guid_field(std::string& value) {
    if (value.empty()) return true;
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else {
            if (!is_hex(value[i])) return false;
            value[i] = ascii_lower(value[i]);
        }
    }
    return true;
}

bool is_valid_node_path(std::string_view value) {
    if (value.empty()) return true;
    std::size_t begin = 0;
    std::size_t segments = 0;
    while (begin < value.size()) {
        if (++segments > 64) return false;
        const std::size_t end = value.find('.', begin);
        const std::size_t length = (end == std::string_view::npos ? value.size() : end) - begin;
        if (length == 0 || length > 7 || (length > 1 && value[begin] == '0')) return false;
        std::uint32_t index = 0;
        for (std::size_t i = 0; i < length; ++i) {
            const char c = value[begin + i];
            if (c < '0' || c > '9') return false;
            index = index * 10 + static_cast<unsigned>(c - '0');
        }
        if (index > 1000000) return false;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return true;
}

std::string binding_key(const GestureBinding& binding) {
    std::string key;
    const auto append = [&key](std::string_view field) {
        key += std::to_string(field.size());
        key.push_back(':');
        key += field;
        key.push_back(';');
    };
    append(binding.gesture);
    append(binding.group);
    append(binding.command_path);
    append(binding.place);
    append(binding.command_guid);
    append(binding.subcommand_guid);
    append(binding.node_path);
    return key;
}

void add_error(BindingParseResult& result, std::size_t line, std::string message) {
    result.valid = false;
    ++result.dropped_rows;
    result.diagnostics.push_back({ line, std::move(message) });
}

bool parse_binding_row(const std::vector<std::string>& fields, BindingSchema schema,
                       GestureBinding& binding, std::string& error) {
    const std::size_t expected = schema == BindingSchema::schema_v2 ? 7 : 4;
    if (fields.size() != expected) {
        error = "expected " + std::to_string(expected) + " fields, got " + std::to_string(fields.size());
        return false;
    }

    binding.gesture = normalize_gesture(fields[0]);
    if (binding.gesture.empty()) {
        error = "empty or malformed gesture";
        return false;
    }
    binding.group = fields[1];
    binding.command_path = fields[2];
    binding.place = fields[3];
    if (!is_valid_command_group(binding.group)) {
        error = "unknown command group";
        return false;
    }
    if (!is_valid_command_place(binding.place)) {
        error = "unknown command place";
        return false;
    }
    if (binding.command_path.empty() || !is_valid_utf8(binding.command_path)) {
        error = "empty or invalid UTF-8 command path";
        return false;
    }

    if (schema == BindingSchema::schema_v2) {
        binding.command_guid = fields[4];
        binding.subcommand_guid = fields[5];
        binding.node_path = fields[6];
        if (!normalize_guid_field(binding.command_guid) ||
            !normalize_guid_field(binding.subcommand_guid)) {
            error = "malformed command/subcommand GUID";
            return false;
        }
        if (!binding.subcommand_guid.empty() && binding.command_guid.empty()) {
            error = "subcommand GUID without command GUID";
            return false;
        }
        if (!is_valid_node_path(binding.node_path)) {
            error = "malformed structural node path";
            return false;
        }
    }
    return true;
}

} // namespace

GestureSettings GestureConfig::defaults() {
    GestureSettings settings;
    settings.stroke_length = 8;
    settings.bindings = {
        { "L", "main", "Playback/Previous", "main_window" },
        { "R", "main", "Playback/Next", "main_window" },
        { "U", "main", "Playback/Volume up", "main_window" },
        { "D", "main", "Playback/Volume down", "main_window" },
    };
    return settings;
}

std::string GestureConfig::serialize(const std::vector<GestureBinding>& bindings) {
    std::string out(kSchemaV2Header);
    out.push_back('\n');
    for (const auto& binding : bindings) {
        if (binding.gesture.empty()) continue;
        out += escape_field(binding.gesture);
        out.push_back('\t');
        out += escape_field(binding.group);
        out.push_back('\t');
        out += escape_field(binding.command_path);
        out.push_back('\t');
        out += escape_field(binding.place);
        out.push_back('\t');
        out += escape_field(binding.command_guid);
        out.push_back('\t');
        out += escape_field(binding.subcommand_guid);
        out.push_back('\t');
        out += escape_field(binding.node_path);
        out.push_back('\n');
    }
    return out;
}

BindingParseResult GestureConfig::deserialize(std::string_view text) {
    BindingParseResult result;
    if (text.empty()) return result;

    std::vector<std::string> fields;
    std::unordered_set<std::string> seen;
    bool first_line = true;
    std::size_t line_number = 0;
    std::size_t begin = 0;

    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(begin, end - begin);
        begin = end + 1;
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (first_line) {
            first_line = false;
            if (line == kSchemaV2Header) {
                result.schema = BindingSchema::schema_v2;
                continue;
            }
            if (line.size() >= 3 && line.substr(0, 3) == "MGH" && line.find('\t') == std::string_view::npos) {
                result.schema = BindingSchema::unknown;
                add_error(result, line_number, "unknown binding schema header");
                return result;
            }
            result.schema = BindingSchema::legacy_v1;
        }
        if (line.empty()) continue;

        if (!split_fields(line, fields)) {
            add_error(result, line_number, "invalid escape or control character");
            continue;
        }

        GestureBinding binding;
        std::string error;
        if (!parse_binding_row(fields, result.schema, binding, error)) {
            add_error(result, line_number, std::move(error));
            continue;
        }

        if (!seen.insert(binding_key(binding)).second) {
            ++result.duplicate_rows;
            result.diagnostics.push_back({ line_number, "exact duplicate ignored; first row has priority" });
            continue;
        }
        result.bindings.push_back(std::move(binding));
    }

    return result;
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

bool is_valid_command_group(std::string_view group_id) {
    for (const auto& entry : command_groups()) {
        if (group_id == entry.first) return true;
    }
    return false;
}

bool is_valid_command_place(std::string_view place_id) {
    for (const auto& entry : command_places()) {
        if (place_id == entry.first) return true;
    }
    return false;
}

const char* group_label(std::string_view group_id) {
    for (const auto& entry : command_groups()) {
        if (group_id == entry.first) return entry.second;
    }
    return "unknown group";
}

std::string group_id_from_label(std::string_view label) {
    for (const auto& entry : command_groups()) {
        if (iequals_ascii(label, entry.second) || iequals_ascii(label, entry.first)) return entry.first;
    }
    return {};
}

const char* place_label(std::string_view place_id) {
    for (const auto& entry : command_places()) {
        if (place_id == entry.first) return entry.second;
    }
    return "unknown place";
}

std::string place_id_from_label(std::string_view label) {
    for (const auto& entry : command_places()) {
        if (iequals_ascii(label, entry.second) || iequals_ascii(label, entry.first)) return entry.first;
    }
    return {};
}

} // namespace mgh
