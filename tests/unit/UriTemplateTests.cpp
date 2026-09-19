// UriTemplateTests — RFC 6570 URI template expansion/matching tests

#include <detail/UriTemplate.hpp>

#include <mcp/test/McpTest.hpp>

#include <cstddef>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace mcp::detail;

namespace {

std::map<std::string, std::string> MakeVars(
    std::initializer_list<std::pair<std::string, std::string>> items) {
    return std::map<std::string, std::string>(items.begin(), items.end());
}

bool Parses(std::string_view tmpl) {
    std::string error;
    return UriTemplate::Parse(tmpl, error).has_value();
}

std::string Expanding(std::string_view tmpl,
                      std::initializer_list<std::pair<std::string, std::string>> items) {
    std::string error;
    const auto parsed = UriTemplate::Parse(tmpl, error);
    if (!parsed.has_value()) {
        return "<parse error: " + error + ">";
    }
    return parsed->Expand(MakeVars(items));
}

std::optional<std::map<std::string, std::string>> Matching(std::string_view tmpl,
                                                           std::string_view uri) {
    std::string error;
    const auto parsed = UriTemplate::Parse(tmpl, error);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->Match(uri);
}

std::string VariablesOf(std::string_view tmpl, std::string_view uri, const std::string& name) {
    const auto matched = Matching(tmpl, uri);
    if (!matched.has_value()) {
        return "<no match>";
    }
    const auto found = matched->find(name);
    return found == matched->end() ? std::string("<missing>") : found->second;
}

}  // namespace

TEST(UriTemplateTest, ParseRejectsUnbalancedBraces) {
    std::string error;
    EXPECT_FALSE(UriTemplate::Parse("{var", error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(UriTemplate::Parse("var}", error).has_value());
    EXPECT_FALSE(UriTemplate::Parse("{var}}", error).has_value());
    EXPECT_FALSE(UriTemplate::Parse("{{var}}", error).has_value());
    EXPECT_FALSE(UriTemplate::Parse("test://template/{id/data", error).has_value());
    EXPECT_TRUE(UriTemplate::Parse("no/variables/here", error).has_value());
}

TEST(UriTemplateTest, ParseRejectsInvalidVarName) {
    EXPECT_FALSE(Parses("{a..b}"));
    EXPECT_FALSE(Parses("{a.}"));
    EXPECT_FALSE(Parses("{a b}"));
    EXPECT_FALSE(Parses("{a-b}"));
    EXPECT_FALSE(Parses("{a%2}"));
    EXPECT_FALSE(Parses("{}"));
    EXPECT_FALSE(Parses("{a,}"));
    // "{.a}" is a valid label expression (label operator + name "a"), so the leading-dot
    // name case is covered through a non-first variable instead.
    EXPECT_FALSE(Parses("{a,.b}"));
    EXPECT_FALSE(Parses("{..}"));
    EXPECT_TRUE(Parses("{a.b}"));
    EXPECT_TRUE(Parses("{a%20b}"));
    EXPECT_TRUE(Parses("{.a}"));
}

TEST(UriTemplateTest, ParseRejectsPrefixModifier) {
    EXPECT_FALSE(Parses("{var:3}"));
    EXPECT_FALSE(Parses("{+path:6}"));
    EXPECT_FALSE(Parses("{/list*,var:2}"));
}

TEST(UriTemplateTest, ParseRejectsTwoMultiSegmentVariables) {
    EXPECT_FALSE(Parses("{+a}{+b}"));
    EXPECT_FALSE(Parses("{+a}{#b}"));
    EXPECT_FALSE(Parses("{+a}/{+b}"));
    EXPECT_FALSE(Parses("{#a}/{+b}"));
    EXPECT_TRUE(Parses("{+a}"));
    EXPECT_TRUE(Parses("{#a}"));
    EXPECT_TRUE(Parses("{+a}/{b}"));
}

TEST(UriTemplateTest, ParseRejectsAdjacentUndelimitedVariables) {
    EXPECT_FALSE(Parses("{a}{b}"));
    EXPECT_FALSE(Parses("{+a}{b}"));
    EXPECT_FALSE(Parses("{a}{+b}"));
    EXPECT_TRUE(Parses("{?a}{?b}"));
    EXPECT_TRUE(Parses("{a}{.b}"));
    EXPECT_TRUE(Parses("{+a}{.b}"));
    EXPECT_TRUE(Parses("{a}{/b}"));
    EXPECT_TRUE(Parses("{a}{?b}"));
    EXPECT_TRUE(Parses("{a}{;b}"));
    EXPECT_TRUE(Parses("{a}x{b}"));
}

TEST(UriTemplateTest, ParseRejectsOverlongTemplate) {
    const std::string at_limit = std::string(kMaxUriTemplateLength, 'a');
    EXPECT_TRUE(Parses(at_limit));

    const std::string overlong = "{" + std::string(kMaxUriTemplateLength, 'a') + "}";
    EXPECT_TRUE(overlong.size() > kMaxUriTemplateLength);
    EXPECT_FALSE(Parses(overlong));
}

TEST(UriTemplateTest, ParseRejectsTooManyVariables) {
    std::string allowed;
    for (std::size_t index = 0; index < kMaxUriTemplateVariables; ++index) {
        if (index > 0) {
            allowed.push_back('/');
        }
        allowed += "{v" + std::to_string(index) + "}";
    }
    EXPECT_TRUE(Parses(allowed));

    EXPECT_FALSE(Parses(allowed + "/{extra}"));
}

TEST(UriTemplateTest, ExpandSimple) {
    EXPECT_EQ(Expanding("{var}", {{"var", "value"}}), std::string("value"));
    EXPECT_EQ(Expanding("{hello}", {{"hello", "Hello World!"}}),
              std::string("Hello%20World%21"));
    EXPECT_EQ(Expanding("{x,y}", {{"x", "1024"}, {"y", "768"}}), std::string("1024,768"));
    EXPECT_EQ(Expanding("{a.b}", {{"a.b", "value"}}), std::string("value"));
    EXPECT_EQ(Expanding("test://template/{id}/data", {{"id", "123"}}),
              std::string("test://template/123/data"));
}

TEST(UriTemplateTest, ExpandReserved) {
    EXPECT_EQ(Expanding("{+path}", {{"path", "/foo/bar"}}), std::string("/foo/bar"));
    EXPECT_EQ(Expanding("{+path}/here", {{"path", "/foo/bar"}}), std::string("/foo/bar/here"));
    EXPECT_EQ(Expanding("{+hello}", {{"hello", "Hello World!"}}), std::string("Hello%20World!"));
    EXPECT_EQ(Expanding("{+x,y}", {{"x", "1024"}, {"y", "768"}}), std::string("1024,768"));
    EXPECT_EQ(Expanding("{#frag}", {{"frag", "section/2"}}), std::string("#section/2"));
    EXPECT_EQ(Expanding("{#x,y}", {{"x", "1024"}, {"y", "768"}}), std::string("#1024,768"));
}

TEST(UriTemplateTest, ExpandPathStyles) {
    EXPECT_EQ(Expanding("{.x,y}", {{"x", "1024"}, {"y", "768"}}), std::string(".1024.768"));
    EXPECT_EQ(Expanding("{/x,y}", {{"x", "1024"}, {"y", "768"}}), std::string("/1024/768"));
    EXPECT_EQ(Expanding("{;x,y}", {{"x", "1024"}, {"y", "768"}}), std::string(";x=1024;y=768"));
    EXPECT_EQ(Expanding("{?x,y}", {{"x", "1024"}, {"y", "768"}}), std::string("?x=1024&y=768"));
    EXPECT_EQ(Expanding("{&x,y}", {{"x", "1024"}, {"y", "768"}}), std::string("&x=1024&y=768"));
    EXPECT_EQ(Expanding("{&x,y,empty}", {{"x", "1024"}, {"y", "768"}, {"empty", ""}}),
              std::string("&x=1024&y=768&empty="));
}

TEST(UriTemplateTest, ExpandHandlesMissingAndEmptyVariables) {
    // RFC 6570 §3.2.2-§3.2.8: undefined variables are skipped, defined-but-empty variables
    // still emit their operator prefix and key.
    EXPECT_EQ(Expanding("{empty}", {{"empty", ""}}), std::string(""));
    EXPECT_EQ(Expanding("{undef}", {}), std::string(""));
    EXPECT_EQ(Expanding("{?empty}", {{"empty", ""}}), std::string("?empty="));
    EXPECT_EQ(Expanding("{?undef}", {}), std::string(""));
    EXPECT_EQ(Expanding("{;empty}", {{"empty", ""}}), std::string(";empty"));
    EXPECT_EQ(Expanding("{;undef}", {}), std::string(""));
    EXPECT_EQ(Expanding("{;x,y,empty}", {{"x", "1024"}, {"y", "768"}, {"empty", ""}}),
              std::string(";x=1024;y=768;empty"));
    EXPECT_EQ(Expanding("{?x,y,empty}", {{"x", "1024"}, {"y", "768"}, {"empty", ""}}),
              std::string("?x=1024&y=768&empty="));
    EXPECT_EQ(Expanding("{;x,y,undef}", {{"x", "1024"}, {"y", "768"}}),
              std::string(";x=1024;y=768"));
    EXPECT_EQ(Expanding("{?x,y,undef}", {{"x", "1024"}, {"y", "768"}}),
              std::string("?x=1024&y=768"));
    EXPECT_EQ(Expanding("{/x,y,undef}", {{"x", "1024"}, {"y", "768"}}), std::string("/1024/768"));
    // the operator prefix is emitted once when at least one variable is defined
    EXPECT_EQ(Expanding("{.empty}", {{"empty", ""}}), std::string("."));
    EXPECT_EQ(Expanding("{/empty}", {{"empty", ""}}), std::string("/"));
    EXPECT_EQ(Expanding("{#empty}", {{"empty", ""}}), std::string("#"));
}

TEST(UriTemplateTest, ExpandAndMatchEmptyValue) {
    EXPECT_EQ(Expanding("{a}", {{"a", ""}}), std::string(""));
    EXPECT_EQ(Expanding("{;p}", {{"p", ""}}), std::string(";p"));
    EXPECT_EQ(Expanding("{?q}", {{"q", ""}}), std::string("?q="));
    EXPECT_EQ(Expanding("{/p}", {{"p", ""}}), std::string("/"));

    EXPECT_EQ(VariablesOf("{a}", "", "a"), std::string(""));
    EXPECT_EQ(VariablesOf("{;p}", ";p", "p"), std::string(""));
    EXPECT_EQ(VariablesOf("{?q}", "?q=", "q"), std::string(""));
    EXPECT_EQ(VariablesOf("{/p}", "/", "p"), std::string(""));

    const auto matched = Matching("{a}", "");
    EXPECT_TRUE(matched.has_value());
    if (matched) {
        EXPECT_EQ(matched->size(), std::size_t(1));
    }
}

TEST(UriTemplateTest, MatchSimpleTemplate) {
    EXPECT_EQ(VariablesOf("test://template/{id}/data", "test://template/123/data", "id"),
              std::string("123"));

    const auto matched = Matching("test://template/{id}/data", "test://template/123/data");
    EXPECT_TRUE(matched.has_value());
    if (matched) {
        EXPECT_EQ(matched->size(), std::size_t(1));
    }

    EXPECT_FALSE(Matching("test://template/{id}/data", "test://template/123").has_value());
    EXPECT_FALSE(Matching("test://template/{id}/data", "test://other/123/data").has_value());
}

TEST(UriTemplateTest, MatchDecodesPctEncodedValues) {
    EXPECT_EQ(VariablesOf("test://template/{id}/data", "test://template/a%2Fb%20c/data", "id"),
              std::string("a/b c"));
    EXPECT_EQ(VariablesOf("{?q}", "?q=a%26b%3Dc", "q"), std::string("a&b=c"));
    EXPECT_EQ(VariablesOf("{+path}", "/a%2Fb", "path"), std::string("/a/b"));
    EXPECT_EQ(VariablesOf("{/a,b}", "/x%20y/z", "a"), std::string("x y"));
}

TEST(UriTemplateTest, MatchRejectsNonMatchingUri) {
    EXPECT_FALSE(Matching("test://template/{id}/data", "test://template/123/other").has_value());
    EXPECT_FALSE(
        Matching("test://template/{id}/data", "test://template/123/data/extra").has_value());
    EXPECT_FALSE(Matching("{/a,b}", "/value").has_value());
    EXPECT_FALSE(Matching("{a}-{b}", "value").has_value());
    EXPECT_FALSE(Matching("{?q}", "q=value").has_value());
    EXPECT_FALSE(Matching("literal", "different").has_value());
    EXPECT_FALSE(Matching("{a}", "with space").has_value());
    EXPECT_TRUE(Matching("literal", "literal").has_value());
    EXPECT_TRUE(Matching("{a}", "plain").has_value());
}

TEST(UriTemplateTest, MatchRespectsGreedyDirection) {
    // a variable before the multi-segment variable takes the first occurrence of its literal
    EXPECT_EQ(VariablesOf("{a}/x/{+rest}", "p/x/q/x/r", "a"), std::string("p"));
    EXPECT_EQ(VariablesOf("{a}/x/{+rest}", "p/x/q/x/r", "rest"), std::string("q/x/r"));

    // only the trailing variable is greedy (its value is the remainder up to the anchored tail);
    // every earlier variable takes the first occurrence of its literal — this is the corrected
    // convention
    EXPECT_EQ(VariablesOf("{a}-x-{b}", "p-x-q-x-r", "a"), std::string("p"));
    EXPECT_EQ(VariablesOf("{a}-x-{b}", "p-x-q-x-r", "b"), std::string("q-x-r"));

    // a variable after the multi-segment variable is greedy again
    EXPECT_EQ(VariablesOf("{+head}/x/{b}", "/a/x/y/x/z", "head"), std::string("/a/x/y"));
    EXPECT_EQ(VariablesOf("{+head}/x/{b}", "/a/x/y/x/z", "b"), std::string("z"));
}

TEST(UriTemplateTest, MatchRejectsOverlongUri) {
    const std::string overlong =
        "test://template/" + std::string(kMaxUriMatchLength, 'a') + "/data";
    EXPECT_TRUE(overlong.size() > kMaxUriMatchLength);
    EXPECT_FALSE(Matching("test://template/{id}/data", overlong).has_value());

    const std::string at_limit =
        "test://template/" + std::string(kMaxUriMatchLength - 100, 'a') + "/data";
    EXPECT_TRUE(at_limit.size() <= kMaxUriMatchLength);
    EXPECT_TRUE(Matching("test://template/{id}/data", at_limit).has_value());
}

TEST(UriTemplateTest, RoundTripExpandThenMatch) {
    const std::pair<std::string_view, std::map<std::string, std::string>> cases[] = {
        {"test://template/{id}/data", {{"id", "123"}}},
        {"{/a,b}", {{"a", "red"}, {"b", "blue"}}},
        {"{?q}", {{"q", "Hello World!"}}},
        {"{+path}", {{"path", "/foo/bar"}}},
        {"{#frag}", {{"frag", "section/2"}}},
        {"{a}-{b}", {{"a", "1"}, {"b", "2"}}},
        {"{;p}", {{"p", "value"}}},
        {"{/p}", {{"p", ""}}},
        {"{a}{.b}", {{"a", "x"}, {"b", "y"}}},
    };

    for (const auto& entry : cases) {
        std::string error;
        const auto parsed = UriTemplate::Parse(entry.first, error);
        EXPECT_TRUE(parsed.has_value());
        if (!parsed) {
            continue;
        }
        const std::string expanded = parsed->Expand(entry.second);
        const auto matched = parsed->Match(expanded);
        EXPECT_TRUE(matched.has_value());
        if (matched) {
            EXPECT_TRUE(*matched == entry.second);
        }
    }
}

TEST(UriTemplateTest, RoundTripWithRepeatedLiteralSeparators) {
    const std::pair<std::string_view, std::map<std::string, std::string>> cases[] = {
        {"{a}-{b}-{c}", {{"a", "1"}, {"b", "2"}, {"c", "3"}}},
        {"{a}/{b}/{c}", {{"a", "p"}, {"b", "q"}, {"c", "r"}}},
        {"{/a,b,c}", {{"a", "x"}, {"b", "y"}, {"c", "z"}}},
    };

    EXPECT_EQ(Expanding("{/a,b,c}", {{"a", "x"}, {"b", "y"}, {"c", "z"}}),
              std::string("/x/y/z"));

    for (const auto& entry : cases) {
        std::string error;
        const auto parsed = UriTemplate::Parse(entry.first, error);
        EXPECT_TRUE(parsed.has_value());
        if (!parsed) {
            continue;
        }
        const std::string expanded = parsed->Expand(entry.second);
        const auto matched = parsed->Match(expanded);
        EXPECT_TRUE(matched.has_value());
        if (matched) {
            EXPECT_TRUE(*matched == entry.second);
        }
    }
}

TEST(UriTemplateTest, MatchMultipleVariables) {
    EXPECT_EQ(VariablesOf("{/a,b}", "/1024/768", "a"), std::string("1024"));
    EXPECT_EQ(VariablesOf("{/a,b}", "/1024/768", "b"), std::string("768"));
    EXPECT_EQ(VariablesOf("{a}-{b}", "left-right", "a"), std::string("left"));
    EXPECT_EQ(VariablesOf("{a}-{b}", "left-right", "b"), std::string("right"));
    EXPECT_EQ(VariablesOf("{?x,y}", "?x=1&y=2", "x"), std::string("1"));
    EXPECT_EQ(VariablesOf("{?x,y}", "?x=1&y=2", "y"), std::string("2"));
    EXPECT_EQ(VariablesOf("{a,b}", "1024,768", "a"), std::string("1024"));
    EXPECT_EQ(VariablesOf("{a,b}", "1024,768", "b"), std::string("768"));
    EXPECT_EQ(VariablesOf("{;x,y}", ";x=1024;y=768", "x"), std::string("1024"));
    EXPECT_EQ(VariablesOf("{;x,y}", ";x=1024;y=768", "y"), std::string("768"));
    EXPECT_EQ(VariablesOf("test://template/{id}/data", "test://template/123/data", "id"),
              std::string("123"));
}

TEST(UriTemplateTest, PatternReturnsOriginalTemplate) {
    std::string error;
    const auto parsed = UriTemplate::Parse("test://template/{id}/data?x={x}", error);
    EXPECT_TRUE(parsed.has_value());
    if (parsed) {
        EXPECT_TRUE(parsed->Pattern() == std::string_view("test://template/{id}/data?x={x}"));
    }
}
