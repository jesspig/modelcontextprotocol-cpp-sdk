// JsonParserTests.cpp — JSON 解析器行为基线测试（simdjson 时代锁定）

#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <string_view>

using namespace mcp;

namespace {

void ExpectParseError(std::string_view json, McpErrorCode expected) {
    EXPECT_THROW(JsonValue::Parse(json), mcp::McpError);
    try {
        JsonValue::Parse(json);
        FAIL() << "should throw";
    } catch (const mcp::McpError& e) {
        EXPECT_EQ(e.Code(), expected);
    }
}

} // namespace

// ── 合法语法 ──
TEST(JsonParserTest, ParsesNull) {
    EXPECT_TRUE(JsonValue::Parse("null").IsNull());
}

TEST(JsonParserTest, ParsesTrue) {
    auto v = JsonValue::Parse("true");
    EXPECT_TRUE(v.IsBool());
    EXPECT_TRUE(v.GetBool());
}

TEST(JsonParserTest, ParsesFalse) {
    auto v = JsonValue::Parse("false");
    EXPECT_TRUE(v.IsBool());
    EXPECT_FALSE(v.GetBool());
}

TEST(JsonParserTest, ParsesInt) {
    auto v = JsonValue::Parse("-17");
    EXPECT_TRUE(v.IsInt());
    EXPECT_EQ(v.GetInt(), -17);
}

TEST(JsonParserTest, ParsesDouble) {
    auto v = JsonValue::Parse("3.14");
    EXPECT_TRUE(v.IsDouble());
    EXPECT_DOUBLE_EQ(v.GetDouble(), 3.14);
}

TEST(JsonParserTest, ParsesString) {
    auto v = JsonValue::Parse(R"("hello")");
    EXPECT_TRUE(v.IsString());
    EXPECT_EQ(v.GetString(), "hello");
}

TEST(JsonParserTest, ParsesArray) {
    auto v = JsonValue::Parse("[1,2,3]");
    EXPECT_TRUE(v.IsArray());
    EXPECT_EQ(v.Size(), 3);
    EXPECT_EQ(v[0].GetInt(), 1);
    EXPECT_EQ(v[2].GetInt(), 3);
}

TEST(JsonParserTest, ParsesObject) {
    auto v = JsonValue::Parse(R"({"a":1,"b":"x"})");
    EXPECT_TRUE(v.IsObject());
    EXPECT_EQ(v.Size(), 2);
    EXPECT_EQ(v.At("a").GetInt(), 1);
    EXPECT_EQ(v.At("b").GetString(), "x");
}

TEST(JsonParserTest, ParsesNestedThreeLevels) {
    auto v = JsonValue::Parse(R"({"a":{"b":[1]}})");
    EXPECT_EQ(v.At("a").At("b")[0].GetInt(), 1);
}

TEST(JsonParserTest, ParsesAllWhitespacePadding) {
    auto v = JsonValue::Parse(" \t\r\n42 \t\r\n");
    EXPECT_TRUE(v.IsInt());
    EXPECT_EQ(v.GetInt(), 42);
}

TEST(JsonParserTest, ParsesWhitespaceBetweenTokens) {
    auto arr = JsonValue::Parse("[ 1 , 2 , 3 ]");
    EXPECT_EQ(arr.Size(), 3);
    auto obj = JsonValue::Parse("{ \"a\" : 1 }");
    EXPECT_EQ(obj.At("a").GetInt(), 1);
}

TEST(JsonParserTest, ParsesEmptyArray) {
    auto v = JsonValue::Parse("[]");
    EXPECT_TRUE(v.IsArray());
    EXPECT_TRUE(v.Empty());
}

TEST(JsonParserTest, ParsesEmptyObject) {
    auto v = JsonValue::Parse("{}");
    EXPECT_TRUE(v.IsObject());
    EXPECT_TRUE(v.Empty());
}

TEST(JsonParserTest, ParsesEscapeSequences) {
    auto v = JsonValue::Parse(R"("test \" \\ \/ \b \f \n \r \t")");
    EXPECT_EQ(v.GetString(), "test \" \\ / \b \f \n \r \t");
}

TEST(JsonParserTest, ParsesSurrogatePair) {
    auto v = JsonValue::Parse(R"("\uD83D\uDE00")");
    EXPECT_EQ(v.GetString(), "\xF0\x9F\x98\x80");
}

TEST(JsonParserTest, DuplicateKeysFirstWins) {
    auto v = JsonValue::Parse(R"({"a":1,"a":2})");
    EXPECT_EQ(v.At("a").GetInt(), 1);
}

// ── 数字合法边界 ──
TEST(JsonParserTest, NumberZero) {
    auto v = JsonValue::Parse("0");
    EXPECT_TRUE(v.IsInt());
    EXPECT_EQ(v.GetInt(), 0);
}

TEST(JsonParserTest, NumberNegativeZero) {
    auto v = JsonValue::Parse("-0");
    EXPECT_TRUE(v.IsInt());
    EXPECT_EQ(v.GetInt(), 0);
}

TEST(JsonParserTest, NumberZeroPointZero) {
    EXPECT_TRUE(JsonValue::Parse("0.0").IsDouble());
}

TEST(JsonParserTest, NumberNegativeExponent) {
    auto v = JsonValue::Parse("-1.5e-3");
    EXPECT_TRUE(v.IsDouble());
    EXPECT_DOUBLE_EQ(v.GetDouble(), -0.0015);
}

TEST(JsonParserTest, NumberUpperExponentPlus) {
    auto v = JsonValue::Parse("1E+2");
    EXPECT_TRUE(v.IsDouble());
    EXPECT_DOUBLE_EQ(v.GetDouble(), 100.0);
}

TEST(JsonParserTest, NumberExponent) {
    auto v = JsonValue::Parse("2.5e3");
    EXPECT_TRUE(v.IsDouble());
    EXPECT_DOUBLE_EQ(v.GetDouble(), 2500.0);
}

TEST(JsonParserTest, NumberInt64Min) {
    auto v = JsonValue::Parse("-9223372036854775808");
    EXPECT_TRUE(v.IsInt());
    EXPECT_EQ(v.GetInt(), std::numeric_limits<int64_t>::min());
}

TEST(JsonParserTest, NumberInt64Max) {
    auto v = JsonValue::Parse("9223372036854775807");
    EXPECT_TRUE(v.IsInt());
    EXPECT_EQ(v.GetInt(), std::numeric_limits<int64_t>::max());
}

TEST(JsonParserTest, NumberMaxDouble) {
    EXPECT_TRUE(JsonValue::Parse("1.7976931348623157e308").IsDouble());
}

TEST(JsonParserTest, NumberMinSubnormal) {
    EXPECT_TRUE(JsonValue::Parse("5e-324").IsDouble());
}

TEST(JsonParserTest, NumberZeroPointOneIsDouble) {
    auto v = JsonValue::Parse("0.1");
    EXPECT_TRUE(v.IsDouble());
    EXPECT_FALSE(v.IsInt());
    EXPECT_DOUBLE_EQ(v.GetDouble(), 0.1);
}

TEST(JsonParserTest, NumberInt42IsInt) {
    auto v = JsonValue::Parse("42");
    EXPECT_TRUE(v.IsInt());
    EXPECT_FALSE(v.IsDouble());
    EXPECT_EQ(v.GetInt(), 42);
}

// ── 数字错误 ──
TEST(JsonParserTest, RejectLeadingZero) {
    ExpectParseError("01", McpErrorCode::ParseError);
}

TEST(JsonParserTest, RejectTrailingDot) {
    ExpectParseError("1.", McpErrorCode::ParseError);
}

TEST(JsonParserTest, RejectLeadingDot) {
    ExpectParseError(".5", McpErrorCode::ParseError);
}

TEST(JsonParserTest, RejectMissingExponentDigits) {
    ExpectParseError("1e", McpErrorCode::ParseError);
}

TEST(JsonParserTest, RejectExponentSignOnly) {
    ExpectParseError("1e+", McpErrorCode::ParseError);
}

TEST(JsonParserTest, RejectDoubleMinus) {
    ExpectParseError("--1", McpErrorCode::ParseError);
}

TEST(JsonParserTest, RejectPlusSign) {
    ExpectParseError("+1", McpErrorCode::ParseError);
}

TEST(JsonParserTest, RejectHexLiteral) {
    ExpectParseError("0x10", McpErrorCode::ParseError);
}

TEST(JsonParserTest, RejectMultipleDecimalPoints) {
    ExpectParseError("1.2.3", McpErrorCode::ParseError);
}

TEST(JsonParserTest, RejectTrailingCommaNumber) {
    ExpectParseError("1,", McpErrorCode::ParseError);
}

// ── 数字超界 ──
TEST(JsonParserTest, Uint64MaxDeserializeFailed) {
    ExpectParseError("18446744073709551615", McpErrorCode::DeserializeFailed);
}

TEST(JsonParserTest, TwoToThe64ParseError) {
    ExpectParseError("18446744073709551616", McpErrorCode::ParseError);
}

TEST(JsonParserTest, Int64MinMinusOneParseError) {
    ExpectParseError("-9223372036854775809", McpErrorCode::ParseError);
}

TEST(JsonParserTest, DoubleOverflowParseError) {
    ExpectParseError("1e400", McpErrorCode::ParseError);
}

// ── 截断输入 ──
TEST(JsonParserTest, EmptyInputParseError) {
    ExpectParseError("", McpErrorCode::ParseError);
}

TEST(JsonParserTest, WhitespaceOnlyParseError) {
    ExpectParseError("   ", McpErrorCode::ParseError);
}

TEST(JsonParserTest, TruncatedObjectParseError) {
    ExpectParseError("{", McpErrorCode::ParseError);
}

TEST(JsonParserTest, TruncatedObjectAfterKeyParseError) {
    ExpectParseError(R"({"a":)", McpErrorCode::ParseError);
}

TEST(JsonParserTest, TruncatedArrayParseError) {
    ExpectParseError("[1,", McpErrorCode::ParseError);
}

TEST(JsonParserTest, TruncatedStringParseError) {
    ExpectParseError(R"("abc)", McpErrorCode::ParseError);
}

TEST(JsonParserTest, TruncatedTrueParseError) {
    ExpectParseError("tru", McpErrorCode::ParseError);
}

TEST(JsonParserTest, TruncatedNullParseError) {
    ExpectParseError("nu", McpErrorCode::ParseError);
}

TEST(JsonParserTest, TruncatedFalseParseError) {
    ExpectParseError("fal", McpErrorCode::ParseError);
}

// ── 非法字符 ──
TEST(JsonParserTest, UnquotedObjectKeyParseError) {
    ExpectParseError("{a:1}", McpErrorCode::ParseError);
}

TEST(JsonParserTest, MissingCommaArrayParseError) {
    ExpectParseError("[1 2]", McpErrorCode::ParseError);
}

TEST(JsonParserTest, TrailingCommaArrayParseError) {
    ExpectParseError("[1,]", McpErrorCode::ParseError);
}

TEST(JsonParserTest, TrailingCommaObjectParseError) {
    ExpectParseError(R"({"a":1,})", McpErrorCode::ParseError);
}

TEST(JsonParserTest, LeadingCommaArrayParseError) {
    ExpectParseError("[,1]", McpErrorCode::ParseError);
}

TEST(JsonParserTest, EmptyKeyObjectParseError) {
    ExpectParseError("{:}", McpErrorCode::ParseError);
}

TEST(JsonParserTest, StrayClosingBracketParseError) {
    ExpectParseError("]", McpErrorCode::ParseError);
}

TEST(JsonParserTest, StrayClosingBraceParseError) {
    ExpectParseError("}", McpErrorCode::ParseError);
}

TEST(JsonParserTest, SingleQuotedStringParseError) {
    ExpectParseError("'a'", McpErrorCode::ParseError);
}

TEST(JsonParserTest, BareTokenParseError) {
    ExpectParseError("abc", McpErrorCode::ParseError);
}

// ── 字符串错误 ──
TEST(JsonParserTest, InvalidEscapeParseError) {
    ExpectParseError(std::string("\"a\\x\""), McpErrorCode::ParseError);
}

TEST(JsonParserTest, LoneHighSurrogateParseError) {
    ExpectParseError(std::string("\"\\uD800\""), McpErrorCode::ParseError);
}

TEST(JsonParserTest, LoneLowSurrogateParseError) {
    ExpectParseError(std::string("\"\\uDC00\""), McpErrorCode::ParseError);
}

TEST(JsonParserTest, TruncatedUnicodeEscapeParseError) {
    ExpectParseError(std::string("\"\\u12\""), McpErrorCode::ParseError);
}

TEST(JsonParserTest, NonHexUnicodeEscapeParseError) {
    ExpectParseError(std::string("\"\\uZZZZ\""), McpErrorCode::ParseError);
}

TEST(JsonParserTest, RawNewlineInStringParseError) {
    ExpectParseError(std::string("\"a\nb\""), McpErrorCode::ParseError);
}

// ── UTF-8 错误 ──
TEST(JsonParserTest, InvalidUtf8ByteParseError) {
    ExpectParseError(std::string("\"\xFF\""), McpErrorCode::ParseError);
}

TEST(JsonParserTest, OverlongUtf8EncodingParseError) {
    ExpectParseError(std::string("\"\xC0\x80\""), McpErrorCode::ParseError);
}

TEST(JsonParserTest, TruncatedMultibyteUtf8ParseError) {
    ExpectParseError(std::string("\"\xE2\x82\""), McpErrorCode::ParseError);
}

// ── 深度边界 ──
TEST(JsonParserTest, DeepNesting512Succeeds) {
    std::string json(512, '[');
    json.append(512, ']');
    EXPECT_NO_THROW(JsonValue::Parse(json));
}

TEST(JsonParserTest, DeepNesting513Fails) {
    std::string json(513, '[');
    json.append(513, ']');
    ExpectParseError(json, McpErrorCode::ParseError);
}

// ── 类型判别 ──
TEST(JsonParserTest, TypeDiscriminationScalars) {
    EXPECT_TRUE(JsonValue::Parse("42").IsInt());
    EXPECT_TRUE(JsonValue::Parse("42.0").IsDouble());
    EXPECT_TRUE(JsonValue::Parse("true").IsBool());
    EXPECT_TRUE(JsonValue::Parse("null").IsNull());
    EXPECT_TRUE(JsonValue::Parse("\"s\"").IsString());
}

TEST(JsonParserTest, TypeDiscriminationContainers) {
    EXPECT_TRUE(JsonValue::Parse("[]").IsArray());
    EXPECT_TRUE(JsonValue::Parse("{}").IsObject());
}

// ── round-trip 幂等 ──
TEST(JsonParserTest, RoundTripDumpIdempotent) {
    auto v1 = JsonValue::Parse(R"({"a":[1,2.5,true,null,"x"]})");
    std::string dump1 = v1.Dump();
    auto v2 = JsonValue::Parse(dump1);
    std::string dump2 = v2.Dump();
    EXPECT_EQ(dump1, dump2);
}
