#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::detail {

inline constexpr std::size_t kMaxUriTemplateLength = 2048;
inline constexpr std::size_t kMaxUriTemplateVariables = 32;
inline constexpr std::size_t kMaxUriMatchLength = 8192;

class UriTemplate {
public:
    static std::optional<UriTemplate> Parse(std::string_view tmpl, std::string& error);

    std::string Expand(const std::map<std::string, std::string>& variables) const;

    std::optional<std::map<std::string, std::string>> Match(std::string_view uri) const;

    std::string_view Pattern() const { return pattern_; }

private:
    enum class Operator { Simple, Reserved, Fragment, Label, Path, PathStyle, Form, FormCont };

    struct Expression {
        Operator op = Operator::Simple;
        std::vector<std::string> names;
    };

    struct Segment {
        bool is_literal = false;
        std::string literal;
        Expression expression;
    };

    struct MatchPart {
        std::string literal;
        std::string name;
        bool optional_equals = false;
        bool preserve = false;
    };

    static bool ParseExpression(std::string_view body, Expression& expression, std::string& error);
    static bool ValidateVarName(std::string_view name);
    static bool IsVarcharChar(char c);
    static bool IsUnreservedChar(char c);
    static bool IsReservedChar(char c);
    static bool IsHexDigit(char c);
    static unsigned HexValue(char c);
    static char HexDigit(unsigned value);
    static std::string EncodeValue(std::string_view value, bool preserve);
    static std::string DecodeValue(std::string_view text);
    static bool IsValidEncodedValue(std::string_view value);
    static bool IsOperatorChar(char c);
    static bool IsFutureOperatorChar(char c);
    static Operator OperatorFromChar(char c);
    static char OperatorPrefix(Operator op);
    static char OperatorSeparator(Operator op);
    static bool HasPrefixOperator(Operator op);
    static bool IsMultiSegmentOperator(Operator op);
    static void AppendExpandedValue(std::string& out, Operator op, std::string_view name,
                                    std::string_view value);
    static std::optional<std::size_t> FindLiteral(std::string_view uri, std::size_t begin,
                                                  std::string_view literal, bool greedy);

    std::vector<MatchPart> BuildMatchParts() const;

    std::string pattern_;
    std::vector<Segment> segments_;
};

inline bool UriTemplate::IsUnreservedChar(char c) {
    const unsigned char value = static_cast<unsigned char>(c);
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' ||
           value == '~';
}

inline bool UriTemplate::IsReservedChar(char c) {
    switch (c) {
        case ':':
        case '/':
        case '?':
        case '#':
        case '[':
        case ']':
        case '@':
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
            return true;
        default:
            return false;
    }
}

inline bool UriTemplate::IsHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

inline unsigned UriTemplate::HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned>(c - 'A' + 10);
    }
    return static_cast<unsigned>(c - 'a' + 10);
}

inline char UriTemplate::HexDigit(unsigned value) {
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('A' + value - 10);
}

inline bool UriTemplate::IsVarcharChar(char c) {
    const unsigned char value = static_cast<unsigned char>(c);
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '_';
}

inline bool UriTemplate::ValidateVarName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    std::size_t index = 0;
    std::size_t group_length = 0;
    while (index < name.size()) {
        const char c = name[index];
        if (c == '.') {
            if (group_length == 0) {
                return false;
            }
            group_length = 0;
            ++index;
            continue;
        }
        if (c == '%') {
            if (index + 2 >= name.size() || !IsHexDigit(name[index + 1]) ||
                !IsHexDigit(name[index + 2])) {
                return false;
            }
            ++group_length;
            index += 3;
            continue;
        }
        if (!IsVarcharChar(c)) {
            return false;
        }
        ++group_length;
        ++index;
    }
    return group_length > 0;
}

inline bool UriTemplate::IsOperatorChar(char c) {
    return c == '+' || c == '#' || c == '.' || c == '/' || c == ';' || c == '?' || c == '&';
}

inline bool UriTemplate::IsFutureOperatorChar(char c) {
    return c == '=' || c == ',' || c == '!' || c == '@' || c == '|';
}

inline UriTemplate::Operator UriTemplate::OperatorFromChar(char c) {
    switch (c) {
        case '+':
            return Operator::Reserved;
        case '#':
            return Operator::Fragment;
        case '.':
            return Operator::Label;
        case '/':
            return Operator::Path;
        case ';':
            return Operator::PathStyle;
        case '?':
            return Operator::Form;
        case '&':
            return Operator::FormCont;
        default:
            return Operator::Simple;
    }
}

inline char UriTemplate::OperatorPrefix(Operator op) {
    switch (op) {
        case Operator::Fragment:
            return '#';
        case Operator::Label:
            return '.';
        case Operator::Path:
            return '/';
        case Operator::PathStyle:
            return ';';
        case Operator::Form:
            return '?';
        case Operator::FormCont:
            return '&';
        default:
            return '\0';
    }
}

inline bool UriTemplate::HasPrefixOperator(Operator op) { return OperatorPrefix(op) != '\0'; }

inline char UriTemplate::OperatorSeparator(Operator op) {
    switch (op) {
        case Operator::Label:
            return '.';
        case Operator::Path:
            return '/';
        case Operator::PathStyle:
            return ';';
        case Operator::Form:
        case Operator::FormCont:
            return '&';
        default:
            return ',';
    }
}

inline bool UriTemplate::IsMultiSegmentOperator(Operator op) {
    return op == Operator::Reserved || op == Operator::Fragment;
}

inline std::string UriTemplate::EncodeValue(std::string_view value, bool preserve) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char c = value[index];
        if (IsUnreservedChar(c) || (preserve && IsReservedChar(c))) {
            out.push_back(c);
            continue;
        }
        if (c == '%' && index + 2 < value.size() && IsHexDigit(value[index + 1]) &&
            IsHexDigit(value[index + 2])) {
            out.append(value.substr(index, 3));
            index += 2;
            continue;
        }
        const unsigned char byte = static_cast<unsigned char>(c);
        out.push_back('%');
        out.push_back(HexDigit(byte >> 4));
        out.push_back(HexDigit(byte & 0x0Fu));
    }
    return out;
}

inline std::string UriTemplate::DecodeValue(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size() && IsHexDigit(text[index + 1]) &&
            IsHexDigit(text[index + 2])) {
            const unsigned high = HexValue(text[index + 1]);
            const unsigned low = HexValue(text[index + 2]);
            out.push_back(static_cast<char>((high << 4) | low));
            index += 2;
            continue;
        }
        out.push_back(text[index]);
    }
    return out;
}

inline bool UriTemplate::IsValidEncodedValue(std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char c = value[index];
        if (c == '%') {
            if (index + 2 >= value.size() || !IsHexDigit(value[index + 1]) ||
                !IsHexDigit(value[index + 2])) {
                return false;
            }
            index += 2;
            continue;
        }
        if (!IsUnreservedChar(c)) {
            return false;
        }
    }
    return true;
}

inline bool UriTemplate::ParseExpression(std::string_view body, Expression& expression,
                                         std::string& error) {
    if (body.empty()) {
        error = "empty uri template expression";
        return false;
    }
    std::size_t position = 0;
    if (IsOperatorChar(body[0])) {
        expression.op = OperatorFromChar(body[0]);
        position = 1;
    } else if (IsFutureOperatorChar(body[0])) {
        error = "unsupported uri template operator";
        return false;
    }
    if (position >= body.size()) {
        error = "uri template expression has no variable name";
        return false;
    }

    while (position <= body.size()) {
        const std::size_t comma = body.find(',', position);
        std::string_view item = body.substr(
            position, comma == std::string_view::npos ? std::string_view::npos : comma - position);
        if (item.empty()) {
            error = "empty uri template variable name";
            return false;
        }
        if (item.back() == '*') {
            item.remove_suffix(1);
        }
        if (item.empty() || !ValidateVarName(item)) {
            error = "invalid uri template variable name";
            return false;
        }
        expression.names.emplace_back(item);
        if (comma == std::string_view::npos) {
            break;
        }
        position = comma + 1;
    }
    return true;
}

inline std::optional<UriTemplate> UriTemplate::Parse(std::string_view tmpl, std::string& error) {
    error.clear();
    if (tmpl.size() > kMaxUriTemplateLength) {
        error = "uri template exceeds maximum length";
        return std::nullopt;
    }

    UriTemplate result;
    result.pattern_.assign(tmpl);

    std::string literal;
    std::size_t variable_count = 0;
    std::size_t multi_segment_count = 0;
    std::size_t position = 0;

    while (position < tmpl.size()) {
        const char c = tmpl[position];
        if (c == '}') {
            error = "unexpected '}' in uri template";
            return std::nullopt;
        }
        if (c != '{') {
            literal.push_back(c);
            ++position;
            continue;
        }

        const std::size_t close = tmpl.find('}', position + 1);
        if (close == std::string_view::npos) {
            error = "unbalanced '{' in uri template";
            return std::nullopt;
        }

        Expression expression;
        if (!ParseExpression(tmpl.substr(position + 1, close - position - 1), expression, error)) {
            return std::nullopt;
        }

        variable_count += expression.names.size();
        if (variable_count > kMaxUriTemplateVariables) {
            error = "uri template has too many variables";
            return std::nullopt;
        }
        if (IsMultiSegmentOperator(expression.op)) {
            ++multi_segment_count;
            if (multi_segment_count > 1) {
                error = "uri template has multiple multi-segment variables";
                return std::nullopt;
            }
        }

        if (literal.empty()) {
            if (!result.segments_.empty() && !result.segments_.back().is_literal &&
                !HasPrefixOperator(expression.op)) {
                error = "adjacent uri template variables without a literal separator";
                return std::nullopt;
            }
        } else {
            Segment literal_segment;
            literal_segment.is_literal = true;
            literal_segment.literal = literal;
            literal.clear();
            result.segments_.push_back(std::move(literal_segment));
        }

        Segment variable_segment;
        variable_segment.expression = std::move(expression);
        result.segments_.push_back(std::move(variable_segment));

        position = close + 1;
    }

    if (!literal.empty()) {
        Segment literal_segment;
        literal_segment.is_literal = true;
        literal_segment.literal = literal;
        result.segments_.push_back(std::move(literal_segment));
    }
    return result;
}

inline void UriTemplate::AppendExpandedValue(std::string& out, Operator op, std::string_view name,
                                             std::string_view value) {
    switch (op) {
        case Operator::PathStyle:
            out.append(name);
            if (!value.empty()) {
                out.push_back('=');
                out += EncodeValue(value, false);
            }
            return;
        case Operator::Form:
        case Operator::FormCont:
            out.append(name);
            out.push_back('=');
            out += EncodeValue(value, false);
            return;
        case Operator::Reserved:
        case Operator::Fragment:
            out += EncodeValue(value, true);
            return;
        default:
            out += EncodeValue(value, false);
            return;
    }
}

inline std::string UriTemplate::Expand(const std::map<std::string, std::string>& variables) const {
    std::string out;
    for (const Segment& segment : segments_) {
        if (segment.is_literal) {
            out += segment.literal;
            continue;
        }

        const Expression& expression = segment.expression;
        std::vector<std::size_t> defined;
        defined.reserve(expression.names.size());
        for (std::size_t index = 0; index < expression.names.size(); ++index) {
            if (variables.find(expression.names[index]) != variables.end()) {
                defined.push_back(index);
            }
        }
        if (defined.empty()) {
            continue;
        }

        const char prefix = OperatorPrefix(expression.op);
        if (prefix != '\0') {
            out.push_back(prefix);
        }
        const char separator = OperatorSeparator(expression.op);
        for (std::size_t slot = 0; slot < defined.size(); ++slot) {
            if (slot > 0) {
                out.push_back(separator);
            }
            const std::string& name = expression.names[defined[slot]];
            AppendExpandedValue(out, expression.op, name, variables.at(name));
        }
    }
    return out;
}

inline std::vector<UriTemplate::MatchPart> UriTemplate::BuildMatchParts() const {
    std::vector<MatchPart> parts;
    std::string pending;
    for (const Segment& segment : segments_) {
        if (segment.is_literal) {
            pending += segment.literal;
            continue;
        }

        const Expression& expression = segment.expression;
        const char prefix = OperatorPrefix(expression.op);
        if (prefix != '\0') {
            pending.push_back(prefix);
        }
        const bool multi_segment = IsMultiSegmentOperator(expression.op);
        for (std::size_t index = 0; index < expression.names.size(); ++index) {
            if (index > 0) {
                pending.push_back(OperatorSeparator(expression.op));
            }

            MatchPart part;
            part.name = expression.names[index];
            part.preserve = multi_segment;
            if (expression.op == Operator::PathStyle) {
                pending += part.name;
                part.optional_equals = true;
            } else if (expression.op == Operator::Form || expression.op == Operator::FormCont) {
                pending += part.name;
                pending.push_back('=');
            }

            part.literal = pending;
            pending.clear();
            parts.push_back(std::move(part));
        }
    }

    MatchPart tail;
    tail.literal = pending;
    parts.push_back(std::move(tail));
    return parts;
}

inline std::optional<std::size_t> UriTemplate::FindLiteral(std::string_view uri, std::size_t begin,
                                                           std::string_view literal,
                                                           bool greedy) {
    if (begin > uri.size()) {
        return std::nullopt;
    }
    if (literal.empty()) {
        return greedy ? uri.size() : begin;
    }
    if (literal.size() > uri.size()) {
        return std::nullopt;
    }
    if (greedy) {
        const std::size_t found = uri.rfind(literal, uri.size() - literal.size());
        if (found == std::string_view::npos || found < begin) {
            return std::nullopt;
        }
        return found;
    }
    const std::size_t found = uri.find(literal, begin);
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    return found;
}

inline std::optional<std::map<std::string, std::string>> UriTemplate::Match(
    std::string_view uri) const {
    if (uri.size() > kMaxUriMatchLength) {
        return std::nullopt;
    }

    const std::vector<MatchPart> parts = BuildMatchParts();
    if (parts.size() == 1) {
        if (uri == parts.front().literal) {
            return std::map<std::string, std::string>{};
        }
        return std::nullopt;
    }

    const std::string_view head = parts.front().literal;
    const std::string_view tail = parts.back().literal;
    if (uri.size() < head.size() + tail.size()) {
        return std::nullopt;
    }
    if (uri.compare(0, head.size(), head) != 0) {
        return std::nullopt;
    }
    if (uri.compare(uri.size() - tail.size(), tail.size(), tail) != 0) {
        return std::nullopt;
    }

    const std::size_t variable_count = parts.size() - 1;

    std::map<std::string, std::string> matched;
    std::size_t position = head.size();
    for (std::size_t index = 0; index < variable_count; ++index) {
        const MatchPart& part = parts[index];
        const std::string_view next_literal = parts[index + 1].literal;
        const bool greedy = part.preserve || index + 1 == variable_count;
        const std::optional<std::size_t> literal_position =
            FindLiteral(uri, position, next_literal, greedy);
        if (!literal_position.has_value()) {
            return std::nullopt;
        }

        std::string_view raw = uri.substr(position, *literal_position - position);
        if (part.optional_equals && !raw.empty()) {
            if (raw.front() != '=') {
                return std::nullopt;
            }
            raw.remove_prefix(1);
        }
        if (!part.preserve && !IsValidEncodedValue(raw)) {
            return std::nullopt;
        }
        matched[part.name] = DecodeValue(raw);
        position = *literal_position + next_literal.size();
    }
    if (position != uri.size()) {
        return std::nullopt;
    }
    return matched;
}

}  // namespace mcp::detail
