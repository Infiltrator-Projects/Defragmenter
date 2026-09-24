// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.hpp"

#include <infiltratr/core.h>
#include <infiltratr/escape.h>
#include <infiltratr/utf8.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace defragger {
namespace {

std::string quoted(std::string_view value) {
    const std::string input(value);
    std::size_t required = 0U;
    if (!infiltratr_escape_json(input.c_str(), nullptr, 0U, &required) ||
        required == 0U) {
        throw std::runtime_error("JSON escaping measurement failed");
    }
    std::vector<char> escaped(required);
    if (!infiltratr_escape_json(
            input.c_str(), escaped.data(), escaped.size(), nullptr)) {
        throw std::runtime_error("JSON escaping failed");
    }
    return "\"" + std::string(escaped.data()) + "\"";
}

void append_utf8(std::string& out, std::uint32_t codepoint) {
    char encoded[4];
    std::size_t encoded_length = 0U;
    if (!infiltratr_utf8_encode_codepoint(
            codepoint, encoded, sizeof(encoded), &encoded_length)) {
        throw std::runtime_error("JSON contains an invalid Unicode code point");
    }
    out.append(encoded, encoded_length);
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    Json parse_document() {
        skip_space();
        Json value = parse_value();
        skip_space();
        if (position_ != input_.size())
            fail("trailing data after JSON value");
        return value;
    }

private:
    std::string_view input_;
    std::size_t position_ = 0U;

    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(
            std::string("JSON parse error at byte ") +
            std::to_string(position_) + ": " + message);
    }

    void skip_space() {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' &&
                value != '\r' && value != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char value) {
        if (position_ < input_.size() && input_[position_] == value) {
            ++position_;
            return true;
        }
        return false;
    }

    void literal(std::string_view value) {
        if (input_.substr(position_, value.size()) != value)
            fail("invalid literal");
        position_ += value.size();
    }

    Json parse_value() {
        skip_space();
        if (position_ >= input_.size()) fail("unexpected end of input");
        switch (input_[position_]) {
        case 'n':
            literal("null");
            return Json(nullptr);
        case 't':
            literal("true");
            return Json(true);
        case 'f':
            literal("false");
            return Json(false);
        case '"':
            return Json(parse_string());
        case '[':
            return Json(parse_array());
        case '{':
            return Json(parse_object());
        default:
            if (input_[position_] == '-' ||
                (input_[position_] >= '0' && input_[position_] <= '9')) {
                return Json::number(parse_number());
            }
            fail("unexpected token");
        }
    }

    std::uint32_t parse_hex4() {
        if (position_ + 4U > input_.size())
            fail("truncated Unicode escape");
        std::uint32_t value = 0U;
        for (unsigned index = 0U; index < 4U; ++index) {
            const int digit = hex_value(input_[position_++]);
            if (digit < 0) fail("invalid Unicode escape");
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    std::string parse_string() {
        if (!consume('"')) fail("expected string");
        std::string out;
        while (position_ < input_.size()) {
            const unsigned char value =
                static_cast<unsigned char>(input_[position_++]);
            if (value == '"') return out;
            if (value < 0x20U) fail("control byte in JSON string");
            if (value != '\\') {
                out.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) fail("truncated escape");
            const char escape = input_[position_++];
            switch (escape) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint = parse_hex4();
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (position_ + 2U > input_.size() ||
                        input_[position_] != '\\' ||
                        input_[position_ + 1U] != 'u') {
                        fail("high surrogate without low surrogate");
                    }
                    position_ += 2U;
                    const std::uint32_t low = parse_hex4();
                    if (low < 0xDC00U || low > 0xDFFFU)
                        fail("invalid low surrogate");
                    codepoint = 0x10000U +
                        ((codepoint - 0xD800U) << 10U) +
                        (low - 0xDC00U);
                } else if (codepoint >= 0xDC00U &&
                           codepoint <= 0xDFFFU) {
                    fail("unpaired low surrogate");
                }
                append_utf8(out, codepoint);
                break;
            }
            default:
                fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    std::string parse_number() {
        const std::size_t start = position_;
        if (consume('-') && position_ >= input_.size())
            fail("truncated number");
        if (position_ >= input_.size()) fail("truncated number");

        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() &&
                input_[position_] >= '0' && input_[position_] <= '9') {
                fail("leading zero in number");
            }
        } else {
            if (input_[position_] < '1' || input_[position_] > '9')
                fail("invalid number");
            while (position_ < input_.size() &&
                   input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }

        if (consume('.')) {
            const std::size_t digits = position_;
            while (position_ < input_.size() &&
                   input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) fail("fraction has no digits");
        }

        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t digits = position_;
            while (position_ < input_.size() &&
                   input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) fail("exponent has no digits");
        }
        return std::string(input_.substr(start, position_ - start));
    }

    Json::Array parse_array() {
        if (!consume('[')) fail("expected array");
        skip_space();
        Json::Array values;
        if (consume(']')) return values;
        for (;;) {
            values.push_back(parse_value());
            skip_space();
            if (consume(']')) return values;
            if (!consume(',')) fail("expected comma in array");
            skip_space();
        }
    }

    Json::Object parse_object() {
        if (!consume('{')) fail("expected object");
        skip_space();
        Json::Object values;
        if (consume('}')) return values;
        for (;;) {
            if (position_ >= input_.size() || input_[position_] != '"')
                fail("expected object key");
            std::string key = parse_string();
            skip_space();
            if (!consume(':')) fail("expected colon after object key");
            skip_space();
            auto [it, inserted] =
                values.emplace(std::move(key), parse_value());
            if (!inserted) fail("duplicate object key");
            skip_space();
            if (consume('}')) return values;
            if (!consume(',')) fail("expected comma in object");
            skip_space();
        }
    }
};

} // namespace

Json::Json() noexcept : value_(nullptr) {}
Json::Json(std::nullptr_t) noexcept : value_(nullptr) {}
Json::Json(bool value) noexcept : value_(value) {}
Json::Json(const char* value) : value_(std::string(value == nullptr ? "" : value)) {}
Json::Json(std::string value) : value_(std::move(value)) {}
Json::Json(Array value) : value_(std::move(value)) {}
Json::Json(Object value) : value_(std::move(value)) {}
Json::Json(Number value) : value_(std::move(value)) {}

Json Json::number(std::string text) {
    if (text.empty()) throw std::invalid_argument("empty JSON number");
    std::size_t position = 0U;
    if (text[position] == '-') {
        ++position;
        if (position == text.size())
            throw std::invalid_argument("invalid JSON number");
    }
    if (text[position] == '0') {
        ++position;
        if (position < text.size() &&
            text[position] >= '0' && text[position] <= '9')
            throw std::invalid_argument("invalid JSON number");
    } else {
        if (text[position] < '1' || text[position] > '9')
            throw std::invalid_argument("invalid JSON number");
        while (position < text.size() &&
               text[position] >= '0' && text[position] <= '9')
            ++position;
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        const std::size_t start = position;
        while (position < text.size() &&
               text[position] >= '0' && text[position] <= '9')
            ++position;
        if (start == position)
            throw std::invalid_argument("invalid JSON number");
    }
    if (position < text.size() &&
        (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() &&
            (text[position] == '+' || text[position] == '-'))
            ++position;
        const std::size_t start = position;
        while (position < text.size() &&
               text[position] >= '0' && text[position] <= '9')
            ++position;
        if (start == position)
            throw std::invalid_argument("invalid JSON number");
    }
    if (position != text.size())
        throw std::invalid_argument("invalid JSON number");
    return Json(Number{std::move(text)});
}

Json Json::integer(std::int64_t value) {
    return Json(Number{std::to_string(value)});
}

Json Json::unsigned_integer(std::uint64_t value) {
    return Json(Number{std::to_string(value)});
}

Json Json::real(double value) {
    if (!std::isfinite(value))
        throw std::invalid_argument("JSON cannot represent non-finite values");
    char buffer[64]{};
    const int length = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(buffer))
        throw std::runtime_error("formatting JSON number failed");
    return Json(Number{std::string(buffer, static_cast<std::size_t>(length))});
}

Json Json::parse(std::string_view text) {
    return Parser(text).parse_document();
}

bool Json::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::is_bool() const noexcept { return std::holds_alternative<bool>(value_); }
bool Json::is_number() const noexcept { return std::holds_alternative<Number>(value_); }
bool Json::is_string() const noexcept { return std::holds_alternative<std::string>(value_); }
bool Json::is_array() const noexcept { return std::holds_alternative<Array>(value_); }
bool Json::is_object() const noexcept { return std::holds_alternative<Object>(value_); }

bool Json::boolean() const {
    if (!is_bool()) throw std::runtime_error("JSON value is not boolean");
    return std::get<bool>(value_);
}

std::string_view Json::string() const {
    if (!is_string()) throw std::runtime_error("JSON value is not a string");
    return std::get<std::string>(value_);
}

std::string_view Json::number_text() const {
    if (!is_number()) throw std::runtime_error("JSON value is not numeric");
    return std::get<Number>(value_).text;
}

std::int64_t Json::integer_value() const {
    const auto text = number_text();
    std::int64_t value = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        throw std::runtime_error("JSON number is not a signed integer");
    }
    return value;
}

std::uint64_t Json::unsigned_value() const {
    const auto text = number_text();
    std::uint64_t value = 0U;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        throw std::runtime_error("JSON number is not an unsigned integer");
    }
    return value;
}

double Json::real_value() const {
    const std::string text(number_text());
    double value = 0.0;
    if (!infiltratr_parse_double(text.c_str(), &value)) {
        throw std::runtime_error("JSON number is not a finite real value");
    }
    return value;
}

const Json::Array& Json::array() const {
    if (!is_array()) throw std::runtime_error("JSON value is not an array");
    return std::get<Array>(value_);
}

Json::Array& Json::array() {
    if (!is_array()) throw std::runtime_error("JSON value is not an array");
    return std::get<Array>(value_);
}

const Json::Object& Json::object() const {
    if (!is_object()) throw std::runtime_error("JSON value is not an object");
    return std::get<Object>(value_);
}

Json::Object& Json::object() {
    if (!is_object()) throw std::runtime_error("JSON value is not an object");
    return std::get<Object>(value_);
}

const Json* Json::find(std::string_view key) const noexcept {
    if (!is_object()) return nullptr;
    const auto& values = std::get<Object>(value_);
    const auto found = values.find(key);
    return found == values.end() ? nullptr : &found->second;
}

Json* Json::find(std::string_view key) noexcept {
    if (!is_object()) return nullptr;
    auto& values = std::get<Object>(value_);
    const auto found = values.find(key);
    return found == values.end() ? nullptr : &found->second;
}

const Json& Json::at(std::string_view key) const {
    const Json* value = find(key);
    if (value == nullptr)
        throw std::runtime_error("JSON object is missing key " + std::string(key));
    return *value;
}

Json& Json::at(std::string_view key) {
    Json* value = find(key);
    if (value == nullptr)
        throw std::runtime_error("JSON object is missing key " + std::string(key));
    return *value;
}

std::string Json::string_or(std::string_view fallback) const {
    return is_string() ? std::string(string()) : std::string(fallback);
}

std::uint64_t Json::unsigned_or(std::uint64_t fallback) const {
    if (!is_number()) return fallback;
    try { return unsigned_value(); } catch (...) { return fallback; }
}

std::int64_t Json::integer_or(std::int64_t fallback) const {
    if (!is_number()) return fallback;
    try { return integer_value(); } catch (...) { return fallback; }
}

double Json::real_or(double fallback) const {
    if (!is_number()) return fallback;
    try { return real_value(); } catch (...) { return fallback; }
}

bool Json::bool_or(bool fallback) const {
    return is_bool() ? boolean() : fallback;
}

std::string Json::dump() const {
    if (is_null()) return "null";
    if (is_bool()) return boolean() ? "true" : "false";
    if (is_number()) return std::string(number_text());
    if (is_string()) return quoted(string());
    if (is_array()) {
        std::string out = "[";
        bool first = true;
        for (const auto& value : array()) {
            if (!first) out += ',';
            first = false;
            out += value.dump();
        }
        out += ']';
        return out;
    }

    std::string out = "{";
    bool first = true;
    for (const auto& [key, value] : object()) {
        if (!first) out += ',';
        first = false;
        out += quoted(key);
        out += ':';
        out += value.dump();
    }
    out += '}';
    return out;
}

} // namespace defragger
