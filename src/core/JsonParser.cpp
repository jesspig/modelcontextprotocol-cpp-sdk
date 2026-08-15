// JsonParser.cpp — 自研递归下降 JSON 解析器（替换 simdjson）

#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>

#include <charconv>
#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace mcp::detail::json {

class Parser {
public:
    static constexpr int kMaxDepth = 512;

    explicit Parser(std::string_view input) : input_(input) {}

    JsonValue ParseDocument() {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();
        if (pos_ != input_.size()) Fail("trailing characters");
        return value;
    }

private:
    [[noreturn]] void Fail(const std::string& reason) const {
        throw McpError(McpErrorCode::ParseError,
            "JSON parse error: " + reason + " at offset " + std::to_string(pos_));
    }

    double ParseFloatFallback(std::string_view token) {
        std::string adjusted(token);
        struct lconv* lc = std::localeconv();
        const char* dp = lc == nullptr ? "." : lc->decimal_point;
        if (dp == nullptr || dp[0] == '\0') dp = ".";
        size_t dp_len = std::strlen(dp);
        if (dp_len != 1 || dp[0] != '.') {
            std::string converted;
            converted.reserve(adjusted.size());
            for (char c : adjusted) {
                if (c == '.') {
                    converted.append(dp, dp_len);
                } else {
                    converted.push_back(c);
                }
            }
            adjusted = std::move(converted);
        }
        char* end = nullptr;
        errno = 0;
        double value = std::strtod(adjusted.c_str(), &end);
        if (end != adjusted.c_str() + adjusted.size()) Fail("invalid number");
        if (errno == ERANGE) {
            if (value == 0.0 || std::abs(value) < std::numeric_limits<double>::min()) {
                return value;
            }
            Fail("number out of range");
        }
        return value;
    }

    static bool IsDigit(char c) {
        return c >= '0' && c <= '9';
    }

    void SkipWhitespace() {
        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    JsonValue ParseValue() {
        if (pos_ >= input_.size()) Fail("unexpected end of input");
        char c = input_[pos_];
        switch (c) {
        case '{':
            return ParseObject();
        case '[':
            return ParseArray();
        case '"':
            return JsonValue(ParseString());
        case 't':
        case 'f':
        case 'n':
            return ParseLiteral();
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return ParseNumber();
        default:
            Fail("unexpected character");
        }
    }

    static size_t EstimateElementCount(std::string_view input, size_t from) {
        static constexpr size_t kHintScanLimit = 4096;
        size_t end = from + kHintScanLimit;
        if (end > input.size()) end = input.size();
        size_t count = 1;
        int depth = 0;
        size_t i = from;
        while (i < end) {
            char c = input[i];
            if (c == '"') {
                ++i;
                while (i < end) {
                    char sc = input[i];
                    if (sc == '\\') {
                        i += 2;
                        continue;
                    }
                    ++i;
                    if (sc == '"') break;
                }
                continue;
            }
            if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (depth == 0) break;
                --depth;
            } else if (c == ',' && depth == 0) {
                ++count;
            }
            ++i;
        }
        return count;
    }

    JsonValue ParseObject() {
        if (++depth_ > kMaxDepth) Fail("nesting depth exceeds " + std::to_string(kMaxDepth));
        ++pos_; // consume '{'
        JsonValue::Object obj;
        while (true) {
            SkipWhitespace();
            if (pos_ >= input_.size()) Fail("unexpected end of object");
            char c = input_[pos_];
            if (c == '}') {
                ++pos_;
                break;
            }
            if (c != '"') Fail("expected object key");
            std::string key = ParseString();
            SkipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != ':') Fail("expected ':'");
            ++pos_;
            SkipWhitespace();
            obj.emplace(std::move(key), ParseValue());
            SkipWhitespace();
            if (pos_ >= input_.size()) Fail("unexpected end of object");
            c = input_[pos_];
            if (c == ',') {
                ++pos_;
                SkipWhitespace();
                if (pos_ >= input_.size()) Fail("unexpected end of object");
                if (input_[pos_] == '}') Fail("trailing comma");
                continue;
            }
            if (c == '}') {
                ++pos_;
                break;
            }
            Fail("expected ',' or '}'");
        }
        --depth_;
        return JsonValue(std::move(obj));
    }

    JsonValue ParseArray() {
        if (++depth_ > kMaxDepth) Fail("nesting depth exceeds " + std::to_string(kMaxDepth));
        ++pos_; // consume '['
        JsonValue::Array arr;
        arr.reserve(EstimateElementCount(input_, pos_));
        while (true) {
            SkipWhitespace();
            if (pos_ >= input_.size()) Fail("unexpected end of array");
            char c = input_[pos_];
            if (c == ']') {
                ++pos_;
                break;
            }
            arr.push_back(ParseValue());
            SkipWhitespace();
            if (pos_ >= input_.size()) Fail("unexpected end of array");
            c = input_[pos_];
            if (c == ',') {
                ++pos_;
                SkipWhitespace();
                if (pos_ >= input_.size()) Fail("unexpected end of array");
                if (input_[pos_] == ']') Fail("trailing comma");
                continue;
            }
            if (c == ']') {
                ++pos_;
                break;
            }
            Fail("expected ',' or ']'");
        }
        --depth_;
        return JsonValue(std::move(arr));
    }

    std::string ParseString() {
        ++pos_; // consume '"'
        std::string result;
        while (true) {
            size_t start = pos_;
            while (pos_ < input_.size()) {
                unsigned char c = static_cast<unsigned char>(input_[pos_]);
                if (c == '"' || c == '\\') break;
                if (c < 0x20) Fail("unescaped control character");
                if (c < 0x80) {
                    ++pos_;
                    continue;
                }
                pos_ += ValidateUtf8Sequence();
            }
            size_t seg_len = pos_ - start;
            if (result.empty() && seg_len > 0) result.reserve(seg_len);
            result.append(input_.data() + start, seg_len);
            if (pos_ >= input_.size()) Fail("unterminated string");
            if (input_[pos_] == '"') {
                ++pos_;
                return result;
            }
            ++pos_; // consume '\\'
            if (pos_ >= input_.size()) Fail("unterminated string");
            char e = input_[pos_];
            switch (e) {
            case '"': result.push_back('"'); ++pos_; break;
            case '\\': result.push_back('\\'); ++pos_; break;
            case '/': result.push_back('/'); ++pos_; break;
            case 'b': result.push_back('\b'); ++pos_; break;
            case 'f': result.push_back('\f'); ++pos_; break;
            case 'n': result.push_back('\n'); ++pos_; break;
            case 'r': result.push_back('\r'); ++pos_; break;
            case 't': result.push_back('\t'); ++pos_; break;
            case 'u': ParseUnicodeEscape(result); break;
            default: Fail("invalid escape sequence");
            }
        }
    }

    void ParseUnicodeEscape(std::string& result) {
        ++pos_; // consume 'u'
        uint32_t cp = ParseHex4();
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // 高代理必须紧跟 \u + 低代理
            if (pos_ + 1 >= input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
                Fail("unpaired high surrogate");
            }
            pos_ += 2;
            uint32_t lo = ParseHex4();
            if (lo < 0xDC00 || lo > 0xDFFF) Fail("invalid low surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            Fail("unpaired low surrogate");
        }
        AppendUtf8(result, cp);
    }

    uint32_t ParseHex4() {
        if (pos_ + 4 > input_.size()) Fail("truncated unicode escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = input_[pos_ + i];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v += static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v += static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v += static_cast<uint32_t>(c - 'A' + 10);
            } else {
                Fail("invalid hex digit");
            }
        }
        pos_ += 4;
        return v;
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // 严格 UTF-8 校验：拒绝 overlong、代理区编码、截断与孤立续字节，返回序列总字节数
    size_t ValidateUtf8Sequence() const {
        size_t p = pos_;
        unsigned char b0 = static_cast<unsigned char>(input_[p]);
        size_t cont = 0;
        unsigned char lo = 0x80;
        unsigned char hi = 0xBF;
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            cont = 1;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            cont = 2;
            if (b0 == 0xE0) lo = 0xA0;       // 拒绝 overlong
            else if (b0 == 0xED) hi = 0x9F;  // 拒绝代理区 ED A0-BF
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            cont = 3;
            if (b0 == 0xF0) lo = 0x90;       // 拒绝 overlong
            else if (b0 == 0xF4) hi = 0x8F;  // 拒绝超出 U+10FFFF
        } else {
            Fail("invalid UTF-8 leading byte");
        }
        if (p + cont >= input_.size()) Fail("truncated UTF-8 sequence");
        unsigned char b1 = static_cast<unsigned char>(input_[p + 1]);
        if (b1 < lo || b1 > hi) Fail("invalid UTF-8 sequence");
        for (size_t i = 2; i <= cont; ++i) {
            unsigned char c = static_cast<unsigned char>(input_[p + i]);
            if ((c & 0xC0) != 0x80) Fail("invalid UTF-8 continuation byte");
        }
        return cont + 1;
    }

    // 两阶段：先扫描 token 并完成语法校验，再按形态分类解析
    JsonValue ParseNumber() {
        size_t start = pos_;
        size_t i = pos_;
        bool is_float = false;
        if (input_[i] == '-') ++i;
        if (i >= input_.size() || !IsDigit(input_[i])) Fail("invalid number");
        if (input_[i] == '0') {
            ++i;
            if (i < input_.size() && IsDigit(input_[i])) Fail("leading zeros");
        } else {
            while (i < input_.size() && IsDigit(input_[i])) ++i;
        }
        if (i < input_.size() && input_[i] == '.') {
            is_float = true;
            ++i;
            if (i >= input_.size() || !IsDigit(input_[i])) Fail("invalid fraction");
            while (i < input_.size() && IsDigit(input_[i])) ++i;
        }
        if (i < input_.size() && (input_[i] == 'e' || input_[i] == 'E')) {
            is_float = true;
            ++i;
            if (i < input_.size() && (input_[i] == '+' || input_[i] == '-')) ++i;
            if (i >= input_.size() || !IsDigit(input_[i])) Fail("invalid exponent");
            while (i < input_.size() && IsDigit(input_[i])) ++i;
        }
        std::string_view token = input_.substr(start, i - start);
        pos_ = i;
        return ParseNumberToken(token, is_float);
    }

    JsonValue ParseNumberToken(std::string_view token, bool is_float) {
        const char* first = token.data();
        const char* last = first + token.size();
        if (!is_float) {
            int64_t int_val = 0;
            auto res = std::from_chars(first, last, int_val);
            if (res.ec == std::errc()) return JsonValue(int_val);
            if (res.ec == std::errc::result_out_of_range) {
                if (!token.empty() && token[0] == '-') Fail("integer out of range");
                uint64_t uint_val = 0;
                auto res2 = std::from_chars(first, last, uint_val);
                if (res2.ec == std::errc()) {
                    if (uint_val > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                        throw McpError(McpErrorCode::DeserializeFailed,
                            std::string("JSON parse error: uint64 value out of int64 range: ") + std::to_string(uint_val));
                    }
                    return JsonValue(static_cast<int64_t>(uint_val));
                }
                Fail("integer out of range");
            }
            Fail("invalid number");
        }
#if defined(_LIBCPP_VERSION)
        // libc++ 下 std::from_chars 浮点重载为 deleted，任何形式的编译期门控都会触发
        // "call to deleted function"，必须用预处理宏彻底移除调用
        return JsonValue(ParseFloatFallback(token));
#else
        double double_val = 0;
        auto res = std::from_chars(first, last, double_val);
        if (res.ec == std::errc()) return JsonValue(double_val);
        if (res.ec == std::errc::result_out_of_range) Fail("number out of range");
        Fail("invalid number");
#endif
    }

    JsonValue ParseLiteral() {
        if (input_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return JsonValue(nullptr);
        }
        if (input_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return JsonValue(true);
        }
        if (input_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return JsonValue(false);
        }
        Fail("invalid literal");
    }

    std::string_view input_;
    size_t pos_ = 0;
    int depth_ = 0;
};

JsonValue ParseDocument(std::string_view json) {
    return Parser(json).ParseDocument();
}

} // namespace mcp::detail::json
