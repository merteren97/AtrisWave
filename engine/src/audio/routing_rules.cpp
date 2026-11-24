#include <atriswave/audio/routing_rules.hpp>
#include <atriswave/audio/com_utils.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace atriswave::audio {
namespace {

std::optional<size_t> find_key_value_start(std::string_view obj, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t search_from = 0;
    while (true) {
        const size_t kp = obj.find(needle, search_from);
        if (kp == std::string_view::npos) {
            return std::nullopt;
        }
        size_t pos = kp + needle.size();
        while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) {
            ++pos;
        }
        if (pos < obj.size() && obj[pos] == ':') {
            ++pos;
            while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) {
                ++pos;
            }
            return pos;
        }
        search_from = kp + needle.size();
    }
}

std::optional<std::string> parse_str_field(std::string_view obj, std::string_view key) {
    const auto pos_opt = find_key_value_start(obj, key);
    if (!pos_opt.has_value()) {
        return std::nullopt;
    }
    size_t pos = *pos_opt;
    if (pos >= obj.size() || obj[pos] != '"') {
        return std::nullopt;
    }
    ++pos;

    std::string out;
    while (pos < obj.size() && obj[pos] != '"') {
        if (obj[pos] == '\\' && (pos + 1) < obj.size()) {
            ++pos;
            switch (obj[pos]) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            default: out.push_back(obj[pos]); break;
            }
        } else {
            out.push_back(obj[pos]);
        }
        ++pos;
    }
    if (pos >= obj.size()) {
        return std::nullopt;
    }
    return out;
}

std::optional<uint32_t> parse_uint_field(std::string_view obj, std::string_view key) {
    const auto pos_opt = find_key_value_start(obj, key);
    if (!pos_opt.has_value()) {
        return std::nullopt;
    }
    size_t pos = *pos_opt;
    if (pos >= obj.size() || !std::isdigit(static_cast<unsigned char>(obj[pos]))) {
        return std::nullopt;
    }
    uint64_t v = 0;
    while (pos < obj.size() && std::isdigit(static_cast<unsigned char>(obj[pos]))) {
        v = v * 10ULL + static_cast<uint64_t>(obj[pos] - '0');
        ++pos;
    }
    return static_cast<uint32_t>(v);
}

std::optional<bool> parse_bool_field(std::string_view obj, std::string_view key) {
    const auto pos_opt = find_key_value_start(obj, key);
    if (!pos_opt.has_value()) {
        return std::nullopt;
    }
    const auto tail = obj.substr(*pos_opt);
    if (tail.starts_with("true")) return true;
    if (tail.starts_with("false")) return false;
    return std::nullopt;
}

} // namespace

RoutingRulesStore::RoutingRulesStore() {
    if (const auto def = default_storage_path(); def.has_value()) {
        storage_path_ = *def;
        std::lock_guard lock(mutex_);
        load_locked();
    }
}

RoutingRulesStore::RoutingRulesStore(std::filesystem::path storage_path)
    : storage_path_(std::move(storage_path)) {
    std::lock_guard lock(mutex_);
    load_locked();
}

std::optional<std::filesystem::path> RoutingRulesStore::default_storage_path() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data)) ||
        local_app_data == nullptr) {
        return std::nullopt;
    }

    std::filesystem::path p(local_app_data);
    CoTaskMemFree(local_app_data);
    p /= L"AtrisWave";
    p /= L"routing_rules.json";
    return p;
}

bool RoutingRulesStore::equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool RoutingRulesStore::set_rule(const RoutingRule& rule) {
    if (rule.process_name.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    bool updated = false;
    for (auto& existing : rules_) {
        if (equals_ignore_case(existing.process_name, rule.process_name)) {
            existing = rule;
            updated = true;
            break;
        }
    }
    if (!updated) {
        rules_.push_back(rule);
    }
    return save_locked();
}

bool RoutingRulesStore::delete_rule(std::string_view process_name) {
    if (process_name.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    const size_t old_size = rules_.size();
    rules_.erase(
        std::remove_if(rules_.begin(), rules_.end(), [&](const RoutingRule& r) {
            return equals_ignore_case(r.process_name, process_name);
        }),
        rules_.end());
    if (rules_.size() == old_size) {
        return false;
    }
    return save_locked();
}

std::vector<RoutingRule> RoutingRulesStore::list_rules() const {
    std::lock_guard lock(mutex_);
    return rules_;
}

std::optional<RoutingRule> RoutingRulesStore::find_rule(std::string_view process_name) const {
    std::lock_guard lock(mutex_);
    for (const auto& r : rules_) {
        if (equals_ignore_case(r.process_name, process_name)) {
            return r;
        }
    }
    return std::nullopt;
}

void RoutingRulesStore::load_locked() {
    rules_.clear();
    if (storage_path_.empty()) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(storage_path_, ec)) {
        return;
    }

    std::ifstream in(storage_path_, std::ios::binary);
    if (!in.is_open()) {
        return;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();

    // Parse object elements inside {"rules":[{...},{...}]} with string/brace tracking
    auto find_matching_brace = [&](size_t start) -> size_t {
        bool in_string = false;
        bool escaped = false;
        int depth = 0;
        for (size_t i = start; i < content.size(); ++i) {
            const char ch = content[i];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    in_string = false;
                }
                continue;
            }
            if (ch == '"') {
                in_string = true;
            } else if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    return i;
                }
            }
        }
        return std::string::npos;
    };

    const size_t rules_arr_pos = content.find("\"rules\"");
    size_t pos = (rules_arr_pos != std::string::npos) ? rules_arr_pos : 0;
    while (true) {
        const size_t obj_start = content.find('{', pos);
        if (obj_start == std::string::npos) {
            break;
        }
        const size_t obj_end = find_matching_brace(obj_start);
        if (obj_end == std::string::npos) {
            break;
        }
        const std::string_view slice(content.data() + obj_start, obj_end - obj_start + 1);
        if (const auto proc = parse_str_field(slice, "process_name"); proc.has_value() && !proc->empty()) {
            RoutingRule rule{};
            rule.process_name = *proc;
            rule.channel_id = parse_uint_field(slice, "channel_id").value_or(1);
            rule.target_device_id = parse_str_field(slice, "target_device_id").value_or("");
            rule.loopback_capture = parse_bool_field(slice, "loopback_capture").value_or(false);
            rules_.push_back(std::move(rule));
            pos = obj_end + 1;
        } else {
            pos = obj_start + 1;
        }
    }
}

bool RoutingRulesStore::save_locked() const {
    if (storage_path_.empty()) {
        return false;
    }

    std::error_code ec;
    if (storage_path_.has_parent_path()) {
        std::filesystem::create_directories(storage_path_.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    std::filesystem::path tmp_path = storage_path_;
    tmp_path += L".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }

        out << "{\"rules\":[";
        for (size_t i = 0; i < rules_.size(); ++i) {
            if (i > 0) {
                out << ',';
            }
            const auto& r = rules_[i];
            out << "{\"process_name\":\"" << json_escape(r.process_name)
                << "\",\"channel_id\":" << r.channel_id
                << ",\"target_device_id\":\"" << json_escape(r.target_device_id)
                << "\",\"loopback_capture\":" << (r.loopback_capture ? "true" : "false") << '}';
        }
        out << "]}\n";
        out.flush();
        if (!out.good()) {
            return false;
        }
    }

    if (MoveFileExW(
            tmp_path.c_str(),
            storage_path_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

} // namespace atriswave::audio
