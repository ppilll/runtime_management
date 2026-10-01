#include "runtime/config_manager.hpp"
#include <cctype>
#include <fstream>
#include <map>
#include <stdexcept>
#include <variant>

namespace runtime {
namespace {

struct Json;
using Object = std::map<std::string, Json>;
using Array = std::vector<Json>;
struct Json : std::variant<std::nullptr_t, bool, long long, std::string, Array, Object> {
    using variant::variant;
};

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}
    Json parse() {
        auto value = parse_value();
        whitespace();
        if (position_ != text_.size()) error("trailing content");
        return value;
    }

private:
    void error(const char* reason) const {
        throw std::runtime_error(std::string("JSON at byte ") + std::to_string(position_) + ": " + reason);
    }
    void whitespace() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
    }
    bool take(char expected) {
        whitespace();
        if (position_ < text_.size() && text_[position_] == expected) { ++position_; return true; }
        return false;
    }
    void require(char expected) { if (!take(expected)) error("unexpected character"); }
    void literal(const char* word) {
        while (*word) {
            if (position_ >= text_.size() || text_[position_++] != *word++) error("invalid literal");
        }
    }
    static void utf8(std::string& result, unsigned codepoint) {
        if (codepoint < 0x80) result.push_back(static_cast<char>(codepoint));
        else if (codepoint < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint < 0x10000) {
            result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }
    unsigned hex4() {
        if (position_ + 4 > text_.size()) error("incomplete unicode escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= c - '0';
            else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
            else error("invalid unicode escape");
        }
        return value;
    }
    std::string parse_string() {
        require('"');
        std::string result;
        while (position_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') return result;
            if (c < 0x20) error("control character in string");
            if (c != '\\') { result.push_back(static_cast<char>(c)); continue; }
            if (position_ >= text_.size()) error("incomplete escape");
            switch (text_[position_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                unsigned codepoint = hex4();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (position_ + 2 > text_.size() || text_[position_++] != '\\' || text_[position_++] != 'u')
                        error("missing low surrogate");
                    const unsigned low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) error("invalid low surrogate");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) error("orphan low surrogate");
                utf8(result, codepoint);
                break;
            }
            default: error("unknown escape");
            }
        }
        error("unterminated string");
        return {};
    }
    Json parse_number() {
        const auto begin = position_;
        if (text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) error("invalid number");
        if (text_[position_] == '0') ++position_;
        else {
            if (text_[position_] < '1' || text_[position_] > '9') error("invalid number");
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E'))
            error("configuration requires integer numbers");
        try { return std::stoll(text_.substr(begin, position_ - begin)); }
        catch (const std::exception&) { error("integer out of range"); }
        return nullptr;
    }
    Json parse_value() {
        whitespace();
        if (position_ >= text_.size()) error("expected value");
        const char c = text_[position_];
        if (c == '"') return parse_string();
        if (c == '{') {
            ++position_;
            Object object;
            if (take('}')) return object;
            do {
                whitespace();
                if (position_ >= text_.size() || text_[position_] != '"') error("expected object key");
                auto key = parse_string();
                require(':');
                if (!object.emplace(std::move(key), parse_value()).second) error("duplicate key");
                if (take('}')) return object;
                require(',');
            } while (true);
        }
        if (c == '[') {
            ++position_;
            Array array;
            if (take(']')) return array;
            do {
                array.push_back(parse_value());
                if (take(']')) return array;
                require(',');
            } while (true);
        }
        if (c == 't') { literal("true"); return true; }
        if (c == 'f') { literal("false"); return false; }
        if (c == 'n') { literal("null"); return nullptr; }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        error("invalid value");
        return nullptr;
    }
    const std::string& text_;
    std::size_t position_ = 0;
};

const Json* field(const Object& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

template <typename T>
T get(const Object& object, const char* key, T default_value) {
    const auto* value = field(object, key);
    if (!value) return default_value;
    if (!std::holds_alternative<T>(*value)) throw std::runtime_error(std::string("invalid ") + key);
    return std::get<T>(*value);
}

std::vector<std::string> strings(const Json& value, const char* name) {
    const auto* array = std::get_if<Array>(&value);
    if (!array) throw std::runtime_error(std::string("invalid ") + name);
    std::vector<std::string> result;
    for (const auto& item : *array) {
        if (!std::holds_alternative<std::string>(item)) throw std::runtime_error(std::string("invalid ") + name);
        result.push_back(std::get<std::string>(item));
    }
    return result;
}

ServiceConfig service(const Json& value) {
    const auto* object = std::get_if<Object>(&value);
    if (!object) throw std::runtime_error("service entry must be an object");
    ServiceConfig config;
    config.service_name = get<std::string>(*object, "service_name", "");
    config.executable = get<std::string>(*object, "executable", "");
    if (config.service_name.empty() || config.executable.empty())
        throw std::runtime_error("service_name and executable are required");
    if (const auto* arguments = field(*object, "arguments")) config.arguments = strings(*arguments, "arguments");
    if (const auto* dependency = field(*object, "dependency")) {
        if (const auto* one = std::get_if<std::string>(dependency)) config.dependency.push_back(*one);
        else config.dependency = strings(*dependency, "dependency");
    }
    config.autostart = get<bool>(*object, "autostart", false);
    const auto startup = get<long long>(*object, "startup_timeout", 15);
    const auto heartbeat = get<long long>(*object, "heartbeat_timeout", 15);
    if (startup < 1 || startup > 3600 || heartbeat < 1 || heartbeat > 3600)
        throw std::runtime_error("timeouts must be between 1 and 3600 seconds");
    config.startup_timeout = std::chrono::seconds(startup);
    config.heartbeat_timeout = std::chrono::seconds(heartbeat);
    const auto policy = get<std::string>(*object, "restart_policy", "never");
    if (policy == "never") config.restart_policy = RestartPolicy::never;
    else if (policy == "on-failure") config.restart_policy = RestartPolicy::on_failure;
    else if (policy == "always") config.restart_policy = RestartPolicy::always;
    else throw std::runtime_error("invalid restart_policy");
    return config;
}

} // namespace

std::vector<ServiceConfig> ConfigManager::load_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open config: " + path);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (contents.size() > 1024 * 1024) throw std::runtime_error("config exceeds 1 MiB");
    const auto root = Parser(contents).parse();
    const auto* object = std::get_if<Object>(&root);
    if (!object) throw std::runtime_error("config root must be an object");
    std::vector<ServiceConfig> result;
    if (const auto* services = field(*object, "services")) {
        const auto* array = std::get_if<Array>(services);
        if (!array) throw std::runtime_error("services must be an array");
        for (const auto& item : *array) result.push_back(service(item));
    } else result.push_back(service(root));
    if (result.empty()) throw std::runtime_error("config must contain a service");
    std::map<std::string, std::size_t> names;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (!names.emplace(result[i].service_name, i).second)
            throw std::runtime_error("duplicate service_name: " + result[i].service_name);
    }
    return result;
}

} // namespace runtime
