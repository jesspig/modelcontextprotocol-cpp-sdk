#include <mcp/client/McpClient.hpp>
#include <mcp/client/SkillVerify.hpp>
#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace mcp;

namespace {

constexpr std::string_view kModernSpecVersion = "2026-07-28";

bool IsModernConformanceRun() {
    const char* version = std::getenv("MCP_CONFORMANCE_PROTOCOL_VERSION");
    return version != nullptr && std::string_view(version) == kModernSpecVersion;
}

#ifdef _WIN32
void AppendUtf8CodePoint(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}
#endif

std::string ReadEnvironmentText(const char* name) {
#ifdef _WIN32
    std::wstring wide_name;
    for (const char* p = name; *p != '\0'; ++p)
        wide_name.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    const wchar_t* wide_value = _wgetenv(wide_name.c_str());
    if (!wide_value)
        return std::string();
    std::string utf8;
    for (size_t i = 0; wide_value[i] != L'\0'; ++i) {
        uint32_t code = static_cast<uint32_t>(wide_value[i]);
        if (code >= 0xD800 && code <= 0xDBFF && wide_value[i + 1] != L'\0') {
            uint32_t low = static_cast<uint32_t>(wide_value[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000U + ((code - 0xD800U) << 10) + (low - 0xDC00U);
                ++i;
            } else {
                code = 0xFFFDU;
            }
        } else if (code >= 0xD800 && code <= 0xDFFF) {
            code = 0xFFFDU;
        }
        AppendUtf8CodePoint(utf8, code);
    }
    return utf8;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

std::vector<std::pair<std::string, JsonValue>> ReadToolCallsContext() {
    std::vector<std::pair<std::string, JsonValue>> calls;
    std::string context_text = ReadEnvironmentText("MCP_CONFORMANCE_CONTEXT");
    if (context_text.empty())
        return calls;
    JsonValue parsed = JsonValue::Parse(JsonValue::SanitizeUtf8(context_text));
    const JsonValue* tool_calls = parsed.Find("toolCalls");
    if (!tool_calls || !tool_calls->IsArray())
        return calls;
    for (const auto& call : tool_calls->GetArray()) {
        const JsonValue* name = call.Find("name");
        if (!name || !name->IsString())
            continue;
        JsonValue arguments(JsonValue::Object{});
        if (const JsonValue* args = call.Find("arguments"))
            arguments = *args;
        calls.emplace_back(name->GetString(), std::move(arguments));
    }
    return calls;
}

const Tool* FindTool(const ListToolsResult& tools, std::string_view name) {
    auto it = std::find_if(tools.tools.begin(), tools.tools.end(),
        [name](const Tool& t) { return t.name == name; });
    return it == tools.tools.end() ? nullptr : &*it;
}

std::unique_ptr<McpClient> ConnectClient(
    const std::string& server_url,
    std::string_view client_name,
    ConnectMode connect_mode,
    std::optional<ClientCapabilities> capabilities,
    bool enable_mrtr_auto_fulfill = false) {
    HttpClientTransportOptions transport_options;
    transport_options.endpoint = server_url;
    StreamableHttpClientTransport http_transport(transport_options);
    auto transport = http_transport.Connect();

    ClientOptions options;
    options.client_info = Implementation{std::string(client_name), "1.0.0"};
    options.connect_mode = connect_mode;
    options.capabilities = std::move(capabilities);
    options.enable_tasks_extension = true;
    options.enable_skills_extension = true;
    if (enable_mrtr_auto_fulfill) {
        ClientOptions::InputRequiredConfig mrtr;
        mrtr.auto_fulfill = true;
        options.input_required_config = mrtr;
    }
    auto client = McpClient::Create(transport, options);
    std::cout << "Successfully connected to MCP server (protocol "
              << client->GetNegotiatedProtocolVersion() << ")" << std::endl;
    return client;
}

void RunBasicClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "test-client", ConnectMode::Legacy,
        ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "Successfully listed tools (" << tools.tools.size() << ")"
              << std::endl;
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunToolsCallClient(const std::string& server_url) {
    ConnectMode mode = IsModernConformanceRun() ? ConnectMode::Auto
                                                : ConnectMode::Legacy;
    auto client = ConnectClient(server_url, "test-client", mode,
        ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "Successfully listed tools (" << tools.tools.size() << ")"
              << std::endl;
    if (FindTool(tools, "add_numbers")) {
        JsonValue arguments(JsonValue::Object{
            {"a", JsonValue(int64_t{5})},
            {"b", JsonValue(int64_t{3})}});
        auto result = client->CallTool("add_numbers", arguments);
        for (const auto& content : result.content) {
            if (auto* text = std::get_if<TextContent>(&content))
                std::cout << "Tool call result: " << text->text << std::endl;
        }
    }
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunRequestMetadataClient(const std::string& server_url) {
    ClientCapabilities capabilities;
    RootsCapability roots_capability;
    roots_capability.list_changed = true;
    capabilities.roots = roots_capability;
    capabilities.sampling = SamplingCapability{};
    capabilities.elicitation = ElicitationCapability{};

    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        capabilities);
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunHttpStandardHeadersClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        ClientCapabilities{});

    auto tools = client->ListTools();
    if (!tools.tools.empty())
        client->CallTool(tools.tools.front().name,
            JsonValue(JsonValue::Object{}));

    auto resources = client->ListResources();
    if (!resources.resources.empty())
        client->ReadResource(resources.resources.front().uri);

    auto prompts = client->ListPrompts();
    if (!prompts.prompts.empty())
        client->GetPrompt(prompts.prompts.front().name,
            JsonValue(JsonValue::Object{}));

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunHttpCustomHeadersClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "listed tools:";
    for (const auto& tool : tools.tools)
        std::cout << " " << tool.name;
    std::cout << std::endl;

    for (const auto& [name, arguments] : ReadToolCallsContext())
        client->CallTool(name, arguments);
    client->Close();
}

void RunHttpInvalidToolHeadersClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "post-exclusion tools:";
    for (const auto& tool : tools.tools)
        std::cout << " " << tool.name;
    std::cout << std::endl;

    JsonValue arguments(JsonValue::Object{{"region", JsonValue("us-west1")}});
    for (const auto& tool : tools.tools) {
        try {
            client->CallTool(tool.name, arguments);
        } catch (const std::exception& e) {
            std::cout << "call " << tool.name << " rejected: " << e.what()
                      << std::endl;
        }
    }
    client->Close();
}

void RunMrtrClient(const std::string& server_url) {
    ClientCapabilities capabilities;
    ElicitationCapability elicitation_capability;
    elicitation_capability.form =
        JsonValue(JsonValue::Object{{"applyDefaults", JsonValue(true)}});
    capabilities.elicitation = elicitation_capability;

    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        capabilities, true);
    client->SetElicitationHandler([](const ElicitRequestParams& params) {
        std::cout << "Fulfilling embedded elicitation request: "
                  << params.message << std::endl;
        ElicitResult result;
        result.action = "accept";
        result.content = JsonValue(JsonValue::Object{});
        return result;
    });

    JsonValue empty_arguments(JsonValue::Object{});
    auto echo_result = client->CallTool("test_mrtr_echo_state", empty_arguments);
    std::cout << "test_mrtr_echo_state done" << std::endl;

    auto no_state_result = client->CallTool("test_mrtr_no_state", empty_arguments);
    std::cout << "test_mrtr_no_state done" << std::endl;

    auto unrelated_result = client->CallTool("test_mrtr_unrelated", empty_arguments);
    std::cout << "test_mrtr_unrelated done" << std::endl;

    try {
        client->CallTool("test_mrtr_no_result_type", empty_arguments);
        std::cout << "test_mrtr_no_result_type done" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "test_mrtr_no_result_type rejected locally (no retry): "
                  << e.what() << std::endl;
    }

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunElicitationDefaultsClient(const std::string& server_url) {
    ClientCapabilities capabilities;
    ElicitationCapability elicitation_capability;
    elicitation_capability.form =
        JsonValue(JsonValue::Object{{"applyDefaults", JsonValue(true)}});
    capabilities.elicitation = elicitation_capability;

    auto client = ConnectClient(server_url, "elicitation-defaults-test-client",
        ConnectMode::Legacy, capabilities);
    client->SetElicitationHandler([](const ElicitRequestParams& params) {
        std::cout << "Received elicitation request: " << params.message
                  << std::endl;
        std::cout << "Accepting with empty content - SDK should apply defaults"
                  << std::endl;
        ElicitResult result;
        result.action = "accept";
        result.content = JsonValue(JsonValue::Object{});
        return result;
    });

    auto tools = client->ListTools();
    if (!FindTool(tools, "test_client_elicitation_defaults"))
        throw std::runtime_error(
            "Test tool not found: test_client_elicitation_defaults");

    auto result = client->CallTool("test_client_elicitation_defaults",
        JsonValue(JsonValue::Object{}));
    std::cout << "Tool result content count: " << result.content.size()
              << std::endl;

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunSSERetryClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "sse-retry-test-client",
        ConnectMode::Legacy, ClientCapabilities{});

    auto tools = client->ListTools();
    if (!FindTool(tools, "test_reconnection"))
        throw std::runtime_error("Test tool not found: test_reconnection");

    std::cout << "Calling test_reconnection tool..." << std::endl;
    auto result = client->CallTool("test_reconnection",
        JsonValue(JsonValue::Object{}));
    std::cout << "Tool result content count: " << result.content.size()
              << std::endl;

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunJsonSchemaRefNoDerefClient(const std::string& server_url) {
    ConnectMode mode = IsModernConformanceRun() ? ConnectMode::Auto
                                                : ConnectMode::Legacy;
    auto client = ConnectClient(server_url, "json-schema-ref-no-deref-client",
        mode, ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "Available tools: " << tools.tools.size() << std::endl;
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunJsonSchema2020_12PreservationClient(const std::string& server_url) {
    ConnectMode mode = IsModernConformanceRun() ? ConnectMode::Auto
                                                : ConnectMode::Legacy;
    auto client = ConnectClient(server_url,
        "json-schema-2020-12-preservation-client", mode, ClientCapabilities{});

    auto tools = client->ListTools();
    std::cout << "Available tools: " << tools.tools.size() << std::endl;

    const Tool* focal = FindTool(tools, "json_schema_2020_12_tool");
    if (!focal)
        throw std::runtime_error(
            "Tool 'json_schema_2020_12_tool' not advertised by the server");
    std::cout << "Observed inputSchema: " << focal->input_schema.Dump()
              << std::endl;

    JsonValue arguments(JsonValue::Object{{"schema", focal->input_schema}});
    auto result = client->CallTool("json_schema_echo", arguments);
    std::cout << "Echo result content count: " << result.content.size()
              << std::endl;

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunSkillsNoPrefetchClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "skills-no-prefetch-client",
        ConnectMode::Legacy, ClientCapabilities{});
    auto skills = client->ListSkills();
    std::cout << "Listed skills (" << skills.skills.size() << ")" << std::endl;
    for (const auto& skill : skills.skills) {
        std::string limits_error;
        if (CheckSkillLimits(skill, limits_error)) {
            std::cout << "Skill limits ok (" << skill.uri << ")" << std::endl;
        } else {
            std::cout << "Skill limits exceeded (" << skill.uri
                      << "): " << limits_error << std::endl;
        }
    }
    std::cout << "Supporting files prefetched: 0" << std::endl;
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunSkillsVerifyClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "skills-verify-client",
        ConnectMode::Legacy, ClientCapabilities{});
    auto skills = client->ListSkills();
    std::cout << "Listed skills (" << skills.skills.size() << ")" << std::endl;
    const Skill* target = nullptr;
    for (const auto& skill : skills.skills) {
        if (skill.uri.size() >= 9 &&
            skill.uri.compare(skill.uri.size() - 9, 9, "/SKILL.md") == 0) {
            target = &skill;
            break;
        }
    }
    if (!target && !skills.skills.empty()) target = &skills.skills.front();
    if (!target)
        throw std::runtime_error("Test server advertised no skills");
    std::string limits_error;
    if (CheckSkillLimits(*target, limits_error)) {
        std::cout << "Skill limits ok (" << target->uri << ")" << std::endl;
    } else {
        std::cout << "Skill limits exceeded (" << target->uri
                  << "): " << limits_error << std::endl;
    }
    std::string frontmatter_error;
    if (VerifyFrontmatter(*target, target->frontmatter, frontmatter_error)) {
        std::cout << "Skill frontmatter self-consistent (" << target->uri << ")"
                  << std::endl;
    } else {
        std::cout << "Skill frontmatter mismatch: " << frontmatter_error
                  << std::endl;
    }
    std::string approval_key = ApprovalKey("conformance", *target);
    std::cout << "Skill approval key: " << approval_key << std::endl;
    if (IsApprovalStillValid(*target, *target)) {
        std::cout << "Skill approval snapshot still valid (" << target->uri << ")"
                  << std::endl;
    } else {
        std::cout << "Skill approval snapshot changed (" << target->uri << ")"
                  << std::endl;
    }
    auto resource = client->ReadResource(target->uri);
    std::cout << "Read skill manifest (" << resource.contents.size()
              << " content blocks), stopping before supporting files"
              << std::endl;
    std::string payload;
    for (const auto& content : resource.contents) {
        if (auto* text = std::get_if<TextResourceContents>(&content)) {
            payload = text->text;
            break;
        }
    }
    if (auto* manifest =
            std::get_if<std::vector<SkillResource>>(&target->resources)) {
        for (const auto& file : *manifest) {
            if (file.uri != target->uri) continue;
            std::string bytes_error;
            if (VerifyFileBytes(file, payload, bytes_error)) {
                std::cout << "Skill manifest digest ok (" << file.uri << ")"
                          << std::endl;
            } else {
                std::cout << "Skill manifest digest mismatch: " << bytes_error
                          << std::endl;
            }
            break;
        }
    }
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

struct RawHttpResult {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string ToLowerAscii(std::string value) {
    for (auto& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string TrimAscii(std::string value) {
    size_t begin = 0;
    while (begin < value.size() &&
        (value[begin] == ' ' || value[begin] == '\t'))
        ++begin;
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t'))
        --end;
    return value.substr(begin, end - begin);
}

bool SplitHttpUrl(const std::string& url, std::string& host,
    std::string& port, std::string& path) {
    constexpr std::string_view kScheme = "http://";
    if (url.size() <= kScheme.size() ||
        url.compare(0, kScheme.size(), kScheme) != 0)
        return false;
    std::string rest = url.substr(kScheme.size());
    size_t slash = rest.find('/');
    std::string authority =
        slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    } else {
        host = authority;
        port = "80";
    }
    return !host.empty() && !port.empty();
}

int ParseStatusCode(std::string_view line) {
    size_t first = line.find(' ');
    if (first == std::string_view::npos) return 0;
    size_t pos = first + 1;
    int code = 0;
    int digits = 0;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        code = code * 10 + (line[pos] - '0');
        ++pos;
        ++digits;
    }
    return digits == 3 ? code : 0;
}

std::string DecodeChunkedBody(const std::string& raw_body) {
    std::string out;
    size_t pos = 0;
    for (;;) {
        size_t eol = raw_body.find("\r\n", pos);
        if (eol == std::string::npos || eol == pos) break;
        std::string size_text = raw_body.substr(pos, eol - pos);
        size_t semi = size_text.find(';');
        if (semi != std::string::npos) size_text.resize(semi);
        size_text = TrimAscii(size_text);
        if (size_text.empty()) break;
        unsigned long chunk_size = 0;
        for (char c : size_text) {
            chunk_size <<= 4;
            if (c >= '0' && c <= '9') chunk_size += static_cast<unsigned long>(c - '0');
            else if (c >= 'a' && c <= 'f') chunk_size += static_cast<unsigned long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') chunk_size += static_cast<unsigned long>(c - 'A' + 10);
            else return out;
        }
        if (chunk_size == 0) break;
        size_t data_begin = eol + 2;
        if (data_begin + chunk_size > raw_body.size()) break;
        out.append(raw_body.data() + data_begin, chunk_size);
        pos = data_begin + chunk_size + 2;
    }
    return out;
}

RawHttpResult RawHttpRequest(const std::string& method, const std::string& url,
    const std::map<std::string, std::string>& extra_headers,
    const std::string& body) {
    RawHttpResult result;
    std::string host, port, path;
    if (!SplitHttpUrl(url, host, port, path)) return result;
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return result;
    SOCKET fd = INVALID_SOCKET;
#else
    int fd = -1;
#endif
    RawHttpResult empty_on_fail;
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        return empty_on_fail;
    }
    for (struct addrinfo* item = addresses; item != nullptr;
        item = item->ai_next) {
#ifdef _WIN32
        fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
        DWORD timeout_ms = 10000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) continue;
        struct timeval timeout_value{};
        timeout_value.tv_sec = 10;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout_value,
            sizeof(timeout_value));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout_value,
            sizeof(timeout_value));
#endif
        if (connect(fd, item->ai_addr, item->ai_addrlen) == 0) break;
#ifdef _WIN32
        closesocket(fd);
        fd = INVALID_SOCKET;
#else
        close(fd);
        fd = -1;
#endif
    }
    freeaddrinfo(addresses);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) {
        WSACleanup();
        return empty_on_fail;
    }
#else
    if (fd < 0) return empty_on_fail;
#endif
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + host;
    if (port != "80") request += ":" + port;
    request += "\r\nConnection: close\r\n";
    for (const auto& [key, value] : extra_headers)
        request += key + ": " + value + "\r\n";
    if (!body.empty())
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n" + body;
    size_t sent = 0;
    bool send_ok = true;
    while (sent < request.size()) {
#ifdef _WIN32
        int chunk = send(fd, request.data() + sent,
            static_cast<int>(request.size() - sent), 0);
#else
        ssize_t chunk =
            send(fd, request.data() + sent, request.size() - sent, 0);
#endif
        if (chunk <= 0) {
            send_ok = false;
            break;
        }
        sent += static_cast<size_t>(chunk);
    }
    std::string raw;
    if (send_ok) {
        char buffer[4096];
        for (;;) {
#ifdef _WIN32
            int chunk = recv(fd, buffer, sizeof(buffer), 0);
#else
            ssize_t chunk = recv(fd, buffer, sizeof(buffer), 0);
#endif
            if (chunk <= 0) break;
            raw.append(buffer, static_cast<size_t>(chunk));
        }
    }
#ifdef _WIN32
    closesocket(fd);
    WSACleanup();
#else
    close(fd);
#endif
    if (raw.empty()) return empty_on_fail;
    size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos) return empty_on_fail;
    std::string head = raw.substr(0, head_end);
    std::string rest = raw.substr(head_end + 4);
    size_t line_end = head.find("\r\n");
    std::string status_line =
        line_end == std::string::npos ? head : head.substr(0, line_end);
    result.status = ParseStatusCode(status_line);
    if (result.status == 0) return empty_on_fail;
    size_t line_begin = line_end == std::string::npos
        ? std::string::npos
        : line_end + 2;
    while (line_begin != std::string::npos && line_begin < head.size()) {
        size_t next_end = head.find("\r\n", line_begin);
        std::string line = head.substr(line_begin,
            next_end == std::string::npos
                ? std::string::npos
                : next_end - line_begin);
        size_t colon = line.find(':');
        if (colon != std::string::npos)
            result.headers[ToLowerAscii(TrimAscii(line.substr(0, colon)))] =
                TrimAscii(line.substr(colon + 1));
        if (next_end == std::string::npos) break;
        line_begin = next_end + 2;
    }
    auto encoding = result.headers.find("transfer-encoding");
    if (encoding != result.headers.end() &&
        ToLowerAscii(encoding->second).find("chunked") != std::string::npos) {
        result.body = DecodeChunkedBody(rest);
        return result;
    }
    auto length = result.headers.find("content-length");
    if (length != result.headers.end()) {
        size_t wanted = 0;
        bool valid = !length->second.empty();
        for (char c : TrimAscii(length->second)) {
            if (c < '0' || c > '9') {
                valid = false;
                break;
            }
            wanted = wanted * 10 + static_cast<size_t>(c - '0');
        }
        if (valid && rest.size() >= wanted) {
            result.body = rest.substr(0, wanted);
            return result;
        }
    }
    result.body = rest;
    return result;
}

std::string RawHttpHeader(const RawHttpResult& result, std::string_view name) {
    auto it = result.headers.find(ToLowerAscii(std::string(name)));
    return it == result.headers.end() ? std::string() : it->second;
}

RawHttpResult ConformanceHttpGet(const std::string& url) {
    return RawHttpRequest("GET", url, {}, std::string());
}

RawHttpResult ConformanceHttpPost(const std::string& url,
    const std::string& content_type, const std::string& body,
    const std::map<std::string, std::string>& extra_headers = {}) {
    std::map<std::string, std::string> headers = extra_headers;
    headers["Content-Type"] = content_type;
    headers["Accept"] = "application/json";
    return RawHttpRequest("POST", url, headers, body);
}

std::string FormEncode(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (unsigned char c : input) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back("0123456789ABCDEF"[c >> 4]);
            result.push_back("0123456789ABCDEF"[c & 0xF]);
        }
    }
    return result;
}

std::string Base64Encode(std::string_view input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((input.size() + 2) / 3 * 4);
    for (size_t i = 0; i < input.size(); i += 3) {
        uint32_t group = static_cast<uint32_t>(
            static_cast<unsigned char>(input[i])) << 16;
        size_t tail = input.size() - i;
        if (tail > 1)
            group |= static_cast<uint32_t>(
                static_cast<unsigned char>(input[i + 1])) << 8;
        if (tail > 2)
            group |= static_cast<uint32_t>(
                static_cast<unsigned char>(input[i + 2]));
        result.push_back(kAlphabet[(group >> 18) & 0x3F]);
        result.push_back(kAlphabet[(group >> 12) & 0x3F]);
        result.push_back(tail > 1 ? kAlphabet[(group >> 6) & 0x3F] : '=');
        result.push_back(tail > 2 ? kAlphabet[group & 0x3F] : '=');
    }
    return result;
}

std::string DecodePercent(std::string_view raw) {
    std::string decoded;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            unsigned int value = 0;
            bool valid = true;
            for (size_t j = 1; j <= 2; ++j) {
                char c = raw[i + j];
                value <<= 4;
                if (c >= '0' && c <= '9')
                    value += static_cast<unsigned int>(c - '0');
                else if (c >= 'A' && c <= 'F')
                    value += static_cast<unsigned int>(c - 'A' + 10);
                else if (c >= 'a' && c <= 'f')
                    value += static_cast<unsigned int>(c - 'a' + 10);
                else valid = false;
            }
            if (valid) {
                decoded.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
            decoded.push_back(raw[i]);
        } else if (raw[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(raw[i]);
        }
    }
    return decoded;
}

bool HasQueryKey(std::string_view url, std::string_view key) {
    size_t query = url.find('?');
    if (query == std::string_view::npos) return false;
    std::string_view params = url.substr(query + 1);
    size_t hash = params.find('#');
    if (hash != std::string_view::npos) params = params.substr(0, hash);
    size_t pos = 0;
    while (pos <= params.size()) {
        size_t end = params.find('&', pos);
        std::string_view pair = params.substr(pos,
            end == std::string_view::npos ? std::string_view::npos : end - pos);
        size_t eq = pair.find('=');
        std::string_view name =
            eq == std::string_view::npos ? pair : pair.substr(0, eq);
        if (DecodePercent(name) == key) return true;
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    return false;
}

std::string QueryValue(std::string_view url, std::string_view key) {
    size_t query = url.find('?');
    if (query == std::string_view::npos) return std::string();
    std::string_view params = url.substr(query + 1);
    size_t hash = params.find('#');
    if (hash != std::string_view::npos) params = params.substr(0, hash);
    size_t pos = 0;
    while (pos <= params.size()) {
        size_t end = params.find('&', pos);
        std::string_view pair = params.substr(pos,
            end == std::string_view::npos ? std::string_view::npos : end - pos);
        size_t eq = pair.find('=');
        std::string_view name =
            eq == std::string_view::npos ? pair : pair.substr(0, eq);
        if (DecodePercent(name) == key)
            return eq == std::string_view::npos
                ? std::string()
                : DecodePercent(pair.substr(eq + 1));
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    return std::string();
}

std::string QuotedChallengeValue(std::string_view header, std::string_view key) {
    std::string quoted = std::string(key) + "=\"";
    size_t pos = header.find(quoted);
    if (pos == std::string_view::npos) return std::string();
    pos += quoted.size();
    size_t end = header.find('"', pos);
    if (end == std::string_view::npos) return std::string();
    return std::string(header.substr(pos, end - pos));
}

std::string OriginOf(const std::string& url) {
    constexpr std::string_view kScheme = "http://";
    size_t path = url.find('/', kScheme.size());
    return path == std::string::npos ? url : url.substr(0, path);
}

std::string TrimmedPath(const std::string& url) {
    std::string origin = OriginOf(url);
    std::string path = url.size() > origin.size()
        ? url.substr(origin.size())
        : std::string();
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

constexpr std::string_view kPrmSuffix = "/.well-known/oauth-protected-resource";
constexpr std::string_view kAsSuffix = "/.well-known/oauth-authorization-server";
constexpr std::string_view kOidcSuffix = "/.well-known/openid-configuration";

std::vector<std::string> Rfc9728PrmUrlCandidates(const std::string& server_url) {
    std::string origin = OriginOf(server_url);
    std::string path = TrimmedPath(server_url);
    std::vector<std::string> candidates;
    candidates.push_back(path.empty() || path == "/"
        ? origin + std::string(kPrmSuffix)
        : origin + std::string(kPrmSuffix) + path);
    std::string root_form = origin + std::string(kPrmSuffix);
    if (candidates.front() != root_form) candidates.push_back(root_form);
    return candidates;
}

std::vector<std::string> AuthorizationServerDocUrlCandidates(
    const std::string& as_base) {
    std::string origin = OriginOf(as_base);
    std::string path = TrimmedPath(as_base);
    std::vector<std::string> candidates;
    candidates.push_back(origin + std::string(kAsSuffix) + path);
    candidates.push_back(origin + std::string(kOidcSuffix) + path);
    if (!path.empty() && path != "/") {
        candidates.push_back(origin + path + std::string(kAsSuffix));
        candidates.push_back(origin + path + std::string(kOidcSuffix));
    }
    std::vector<std::string> unique;
    for (const auto& candidate : candidates) {
        if (std::find(unique.begin(), unique.end(), candidate) == unique.end())
            unique.push_back(candidate);
    }
    return unique;
}

std::string NormalizeAuthResource(std::string value) {
    size_t hash = value.find('#');
    if (hash != std::string::npos) value.resize(hash);
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

JsonValue ReadConformanceContext() {
    std::string text = ReadEnvironmentText("MCP_CONFORMANCE_CONTEXT");
    if (text.empty()) return JsonValue();
    try {
        return JsonValue::Parse(JsonValue::SanitizeUtf8(text));
    } catch (...) {
        return JsonValue();
    }
}

std::string ContextString(const JsonValue& context, std::string_view key) {
    if (!context.IsObject()) return std::string();
    const JsonValue* field = context.Find(key);
    if (!field || !field->IsString()) return std::string();
    return field->GetString();
}

std::string JsonStringField(const JsonValue& document, std::string_view key) {
    if (!document.IsObject()) return std::string();
    const JsonValue* field = document.Find(key);
    if (!field || !field->IsString()) return std::string();
    return field->GetString();
}

std::vector<std::string> JsonStringList(const JsonValue& document,
    std::string_view key) {
    std::vector<std::string> result;
    if (!document.IsObject()) return result;
    const JsonValue* field = document.Find(key);
    if (!field || !field->IsArray()) return result;
    for (const auto& item : field->GetArray())
        if (item.IsString()) result.push_back(item.GetString());
    return result;
}

bool ParseJsonBody(const RawHttpResult& result, JsonValue& document) {
    if (result.status < 200 || result.status >= 300 || result.body.empty())
        return false;
    try {
        document = JsonValue::Parse(result.body);
    } catch (...) {
        return false;
    }
    return document.IsObject();
}

bool FetchAuthorizationServerMetadata(const std::string& as_base,
    JsonValue& metadata) {
    for (const auto& url : AuthorizationServerDocUrlCandidates(as_base)) {
        if (ParseJsonBody(ConformanceHttpGet(url), metadata)) return true;
    }
    return false;
}

struct DiscoveredAuth {
    std::string prm_url;
    std::string prm_resource;
    std::string as_base;
    std::string issuer;
    std::string auth_endpoint;
    std::string token_endpoint;
    std::string registration_endpoint;
    bool iss_supported = false;
    std::vector<std::string> prm_scopes;
    std::vector<std::string> as_scopes;
    std::string challenge_scope;
};

bool DiscoverAuthFlow(const std::string& server_url,
    const std::string& prm_override, DiscoveredAuth& auth) {
    std::string probe_body =
        "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"tools/list\",\"params\":{}}";
    RawHttpResult probe = ConformanceHttpPost(server_url,
        "application/json", probe_body);
    std::string www_auth;
    if (probe.status == 401 || probe.status == 403)
        www_auth = RawHttpHeader(probe, "WWW-Authenticate");
    auth.challenge_scope = QuotedChallengeValue(www_auth, "scope");
    std::string prm_url = prm_override;
    if (prm_url.empty())
        prm_url = QuotedChallengeValue(www_auth, "resource_metadata");
    JsonValue prm;
    if (prm_url.empty()) {
        for (const auto& candidate : Rfc9728PrmUrlCandidates(server_url)) {
            if (ParseJsonBody(ConformanceHttpGet(candidate), prm)) {
                prm_url = candidate;
                break;
            }
        }
    } else if (!ParseJsonBody(ConformanceHttpGet(prm_url), prm)) {
        return false;
    }
    if (prm_url.empty()) return false;
    auth.prm_url = prm_url;
    auth.prm_resource = JsonStringField(prm, "resource");
    auth.prm_scopes = JsonStringList(prm, "scopes_supported");
    std::vector<std::string> servers = JsonStringList(prm,
        "authorization_servers");
    if (servers.empty() || auth.prm_resource.empty()) return false;
    auth.as_base = servers.front();
    while (!auth.as_base.empty() && auth.as_base.back() == '/')
        auth.as_base.pop_back();
    JsonValue metadata;
    if (!FetchAuthorizationServerMetadata(auth.as_base, metadata))
        return false;
    auth.issuer = JsonStringField(metadata, "issuer");
    auth.auth_endpoint = JsonStringField(metadata, "authorization_endpoint");
    auth.token_endpoint = JsonStringField(metadata, "token_endpoint");
    auth.registration_endpoint =
        JsonStringField(metadata, "registration_endpoint");
    auth.as_scopes = JsonStringList(metadata, "scopes_supported");
    const JsonValue* iss_flag =
        metadata.IsObject()
            ? metadata.Find("authorization_response_iss_parameter_supported")
            : nullptr;
    auth.iss_supported =
        iss_flag && iss_flag->IsBool() && iss_flag->GetBool();
    return !auth.issuer.empty() && !auth.auth_endpoint.empty() &&
        !auth.token_endpoint.empty();
}

std::vector<std::string> SelectAuthScopes(const DiscoveredAuth& auth) {
    if (!auth.challenge_scope.empty())
        return {auth.challenge_scope};
    if (!auth.prm_scopes.empty()) return auth.prm_scopes;
    return {};
}

std::string ExtractAuthState(std::string_view url);
std::unique_ptr<McpClient> ConnectBearerClient(const std::string& server_url,
    const std::string& scheme, const std::string& token);
bool ManualRegister(const DiscoveredAuth& auth,
    const std::vector<std::string>& grant_types, std::string& client_id,
    std::string& client_secret);

bool AuthenticateForAttempt(const DiscoveredAuth& auth,
    const std::vector<std::string>& scopes,
    const std::string& preset_client_id, bool decline_bad_iss,
    const std::string& server_url) {
    std::string seen_url;
    std::string client_id = preset_client_id;
    std::string client_secret;
    if (client_id.empty() && !auth.registration_endpoint.empty() &&
        !ManualRegister(auth, {}, client_id, client_secret)) {
        std::cerr << "Dynamic client registration failed before provider "
                     "flow"
                  << std::endl;
        return false;
    }
    OAuthClientOptions oauth_options;
    oauth_options.server_url = auth.as_base;
    if (!auth.prm_resource.empty())
        oauth_options.resource = auth.prm_resource;
    oauth_options.redirect_uri = "http://localhost/callback";
    oauth_options.scopes = scopes;
    if (!client_id.empty()) oauth_options.client_id = client_id;
    if (!client_secret.empty()) oauth_options.client_secret = client_secret;
    oauth_options.authorization_redirect_handler =
        [&seen_url](std::string_view url) {
            seen_url = std::string(url);
        };
    oauth_options.authorization_code_callback =
        [&seen_url, &auth, decline_bad_iss]()
            -> std::optional<AuthorizationCodeResult> {
            RawHttpResult reached = ConformanceHttpGet(seen_url);
            std::string location = RawHttpHeader(reached, "location");
            std::string code = location.empty()
                ? "conformance-test-code"
                : QueryValue(location, "code");
            if (code.empty()) code = "conformance-test-code";
            if (decline_bad_iss) {
                bool has_iss = HasQueryKey(location, "iss");
                std::string iss = QueryValue(location, "iss");
                if (auth.iss_supported) {
                    if (!has_iss || iss != auth.issuer) return std::nullopt;
                } else if (has_iss && iss != auth.issuer) {
                    return std::nullopt;
                }
            }
            AuthorizationCodeResult result;
            result.code = code;
            result.state = ExtractAuthState(seen_url);
            return result;
        };
    auto provider = std::make_shared<OAuthClientProvider>(oauth_options);
    bool authenticated = provider->Authenticate();
    std::string access_token = provider->GetAccessToken();
    if (!authenticated || access_token.empty()) {
        std::cerr << "OAuth provider flow did not yield an access token "
                     "(authenticated="
                  << (authenticated ? "true" : "false") << ", token="
                  << (access_token.empty() ? "absent" : "present") << ")"
                  << std::endl;
        return false;
    }
    auto client = ConnectBearerClient(server_url, "Bearer", access_token);
    try {
        auto tools = client->ListTools();
        std::cout << "OAuth provider flow bearer tools listed ("
                  << tools.tools.size() << ")" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "OAuth provider flow bearer call rejected: " << e.what()
                  << std::endl;
        client->Close();
        return false;
    }
    client->Close();
    return true;
}

bool RunProviderAuthFlow(const std::string& server_url,
    const DiscoveredAuth& auth, const std::vector<std::string>& scopes,
    const std::string& preset_client_id, bool decline_bad_iss, int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (!AuthenticateForAttempt(auth, scopes, preset_client_id,
                decline_bad_iss, server_url)) {
            std::cout << "OAuth provider flow attempt " << (attempt + 1)
                      << " produced no usable token" << std::endl;
            return false;
        }
        std::cout << "OAuth provider flow attempt " << (attempt + 1)
                  << " stored a usable token" << std::endl;
    }
    return true;
}

bool ManualRegister(const DiscoveredAuth& auth,
    const std::vector<std::string>& grant_types, std::string& client_id,
    std::string& client_secret) {
    if (auth.registration_endpoint.empty()) return false;
    std::string body =
        "{\"redirect_uris\":[\"http://localhost/callback\"],"
        "\"client_name\":\"mcp-cpp-client\",\"application_type\":\"native\"";
    if (!grant_types.empty()) {
        body += ",\"grant_types\":[";
        bool first = true;
        for (const auto& grant : grant_types) {
            if (!first) body.push_back(',');
            first = false;
            body += "\"" + grant + "\"";
        }
        body.push_back(']');
    }
    body.push_back('}');
    RawHttpResult response = ConformanceHttpPost(auth.registration_endpoint,
        "application/json", body);
    JsonValue document;
    if (!ParseJsonBody(response, document)) return false;
    client_id = JsonStringField(document, "client_id");
    client_secret = JsonStringField(document, "client_secret");
    return !client_id.empty();
}

bool ManualAuthorize(const DiscoveredAuth& auth, const std::string& client_id,
    const std::string& scope_text, std::string& verifier,
    std::string& location) {
    verifier = pkce::GenerateCodeVerifier();
    std::string url = auth.auth_endpoint + "?response_type=code" +
        "&client_id=" + FormEncode(client_id) +
        "&redirect_uri=" + FormEncode("http://localhost/callback") +
        "&code_challenge=" + FormEncode(pkce::ComputeCodeChallenge(verifier)) +
        "&code_challenge_method=S256" +
        "&state=" + FormEncode(pkce::GenerateCodeVerifier());
    if (!scope_text.empty()) url += "&scope=" + FormEncode(scope_text);
    if (!auth.prm_resource.empty())
        url += "&resource=" + FormEncode(auth.prm_resource);
    RawHttpResult response = ConformanceHttpGet(url);
    if (response.status < 300 || response.status >= 400) return false;
    location = RawHttpHeader(response, "location");
    return !location.empty();
}

std::string EncodeFormFields(
    const std::map<std::string, std::string>& fields) {
    std::string body;
    for (const auto& [key, value] : fields) {
        if (!body.empty()) body.push_back('&');
        body += FormEncode(key) + "=" + FormEncode(value);
    }
    return body;
}

bool ManualToken(const DiscoveredAuth& auth,
    const std::map<std::string, std::string>& fields,
    const std::string& basic_credentials, std::string& access_token) {
    std::map<std::string, std::string> headers;
    if (!basic_credentials.empty())
        headers["Authorization"] = "Basic " + basic_credentials;
    RawHttpResult response = ConformanceHttpPost(auth.token_endpoint,
        "application/x-www-form-urlencoded", EncodeFormFields(fields), headers);
    JsonValue document;
    if (!ParseJsonBody(response, document)) return false;
    access_token = JsonStringField(document, "access_token");
    return !access_token.empty();
}

constexpr int kDpopTokenAttempts = 2;
constexpr int64_t kDpopTokenLifetimeMs = 3600 * 1000;

bool ManualDpopToken(const DiscoveredAuth& auth,
    const std::map<std::string, std::string>& fields,
    const std::shared_ptr<OAuthClientProvider>& provider,
    std::string& access_token) {
    std::string body = EncodeFormFields(fields);
    for (int attempt = 0; attempt < kDpopTokenAttempts; ++attempt) {
        std::string proof =
            provider->BuildDPoPProof("POST", auth.token_endpoint);
        if (proof.empty()) return false;
        std::map<std::string, std::string> headers;
        headers["DPoP"] = std::move(proof);
        RawHttpResult response = ConformanceHttpPost(auth.token_endpoint,
            "application/x-www-form-urlencoded", body, headers);
        if (response.status >= 200 && response.status < 300) {
            JsonValue document;
            if (!ParseJsonBody(response, document)) return false;
            access_token = JsonStringField(document, "access_token");
            return !access_token.empty();
        }
        std::string nonce = RawHttpHeader(response, "DPoP-Nonce");
        if (nonce.empty()) return false;
        provider->ObserveDPoPNonce(nonce);
    }
    return false;
}

void ResetDPoPNonce(const std::shared_ptr<OAuthClientProvider>& provider) {
    provider->SetDPoPNonce(std::string());
}

std::map<std::string, std::string> AuthCodeTokenFields(
    const DiscoveredAuth& auth, const std::string& client_id,
    const std::string& code, const std::string& verifier,
    const std::string& scope_text) {
    std::map<std::string, std::string> fields;
    fields["grant_type"] = "authorization_code";
    fields["code"] = code;
    fields["redirect_uri"] = "http://localhost/callback";
    fields["client_id"] = client_id;
    fields["code_verifier"] = verifier;
    if (!scope_text.empty()) fields["scope"] = scope_text;
    if (!auth.prm_resource.empty()) fields["resource"] = auth.prm_resource;
    return fields;
}

std::unique_ptr<McpClient> ConnectBearerClient(const std::string& server_url,
    const std::string& scheme, const std::string& token) {
    HttpClientTransportOptions transport_options;
    transport_options.endpoint = server_url;
    transport_options.additional_headers["Authorization"] = scheme + " " + token;
    StreamableHttpClientTransport http_transport(transport_options);
    auto transport = http_transport.Connect();
    ClientOptions options;
    options.client_info = Implementation{std::string("test-client"), "1.0.0"};
    options.max_total_timeout = std::chrono::seconds(10);
    options.connect_mode = IsModernConformanceRun() ? ConnectMode::Auto
                                                    : ConnectMode::Legacy;
    options.capabilities = ClientCapabilities{};
    options.enable_tasks_extension = true;
    options.enable_skills_extension = true;
    return McpClient::Create(transport, options);
}

std::unique_ptr<McpClient> ConnectDPoPClient(const std::string& server_url,
    const std::string& token,
    const std::shared_ptr<OAuthClientProvider>& provider) {
    HttpClientTransportOptions transport_options;
    transport_options.endpoint = server_url;
    transport_options.additional_headers["Authorization"] = "DPoP " + token;
    transport_options.apply_per_request_headers =
        [provider](const HttpRequestHeaderContext& context,
                   std::map<std::string, std::string>& headers) {
            provider->ApplyDPoPProofHeader(context, headers);
        };
    transport_options.observe_response_challenge =
        [provider](const HttpResponseChallengeContext& context) {
            provider->ObserveDPoPNonce(context.dpop_nonce);
        };
    StreamableHttpClientTransport http_transport(transport_options);
    auto transport = http_transport.Connect();
    ClientOptions options;
    options.client_info = Implementation{std::string("test-client"), "1.0.0"};
    options.max_total_timeout = std::chrono::seconds(10);
    options.connect_mode = IsModernConformanceRun() ? ConnectMode::Auto
                                                    : ConnectMode::Legacy;
    options.capabilities = ClientCapabilities{};
    options.enable_tasks_extension = true;
    options.enable_skills_extension = true;
    return McpClient::Create(transport, options);
}

bool ListToolsWithBearer(const std::string& server_url,
    const std::string& token) {
    auto client = ConnectBearerClient(server_url, "Bearer", token);
    auto tools = client->ListTools();
    std::cout << "Bearer tools listed (" << tools.tools.size() << ")"
              << std::endl;
    client->Close();
    return true;
}

bool IsAuthScenario(std::string_view name) {
    constexpr std::string_view kAuthPrefix = "auth/";
    return name.size() >= kAuthPrefix.size() &&
        name.substr(0, kAuthPrefix.size()) == kAuthPrefix;
}

std::string ExtractAuthState(std::string_view url) {
    constexpr std::string_view kMarker = "state=";
    size_t pos = url.find(kMarker);
    if (pos == std::string_view::npos) return std::string();
    pos += kMarker.size();
    size_t end = url.find('&', pos);
    std::string_view raw = url.substr(pos,
        end == std::string_view::npos ? std::string_view::npos : end - pos);
    std::string decoded;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            unsigned int value = 0;
            bool valid = true;
            for (size_t j = 1; j <= 2; ++j) {
                char c = raw[i + j];
                value <<= 4;
                if (c >= '0' && c <= '9') value += static_cast<unsigned int>(c - '0');
                else if (c >= 'A' && c <= 'F') value += static_cast<unsigned int>(c - 'A' + 10);
                else if (c >= 'a' && c <= 'f') value += static_cast<unsigned int>(c - 'a' + 10);
                else valid = false;
            }
            if (valid) {
                decoded.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
            decoded.push_back(raw[i]);
        } else if (raw[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(raw[i]);
        }
    }
    return decoded;
}

constexpr std::string_view kCimdClientId =
    "https://conformance-test.local/client-metadata.json";
constexpr std::string_view kJwtBearerGrant =
    "urn:ietf:params:oauth:grant-type:jwt-bearer";
constexpr std::string_view kTokenExchangeGrant =
    "urn:ietf:params:oauth:grant-type:token-exchange";
constexpr std::string_view kIdTokenType = "urn:ietf:params:oauth:token-type:id_token";
constexpr std::string_view kIdJagType = "urn:ietf:params:oauth:token-type:id-jag";

bool FetchAsEndpoints(const std::string& as_base, DiscoveredAuth& auth) {
    auth.as_base = as_base;
    while (!auth.as_base.empty() && auth.as_base.back() == '/')
        auth.as_base.pop_back();
    JsonValue metadata;
    if (!FetchAuthorizationServerMetadata(auth.as_base, metadata))
        return false;
    auth.issuer = JsonStringField(metadata, "issuer");
    auth.auth_endpoint = JsonStringField(metadata, "authorization_endpoint");
    auth.token_endpoint = JsonStringField(metadata, "token_endpoint");
    auth.registration_endpoint =
        JsonStringField(metadata, "registration_endpoint");
    auth.as_scopes = JsonStringList(metadata, "scopes_supported");
    const JsonValue* iss_flag =
        metadata.Find("authorization_response_iss_parameter_supported");
    auth.iss_supported =
        iss_flag && iss_flag->IsBool() && iss_flag->GetBool();
    return !auth.issuer.empty() && !auth.auth_endpoint.empty() &&
        !auth.token_endpoint.empty();
}

bool DriveProviderAuthScenario(const std::string& server_url,
    std::string_view scenario) {
    DiscoveredAuth auth;
    if (!DiscoverAuthFlow(server_url, std::string(), auth)) return false;
    std::cout << "Auth discovery done (" << scenario << ")" << std::endl;
    std::string preset;
    if (scenario == "auth/basic-cimd") preset = std::string(kCimdClientId);
    bool decline = scenario == "auth/iss-supported-missing" ||
        scenario == "auth/iss-wrong-issuer" ||
        scenario == "auth/iss-unexpected" ||
        scenario == "auth/iss-normalized";
    int attempts = scenario == "auth/scope-retry-limit" ? 3 : 1;
    return RunProviderAuthFlow(server_url, auth, SelectAuthScopes(auth),
        preset, decline, attempts);
}

bool DriveRejectScenario(const std::string& server_url,
    std::string_view scenario) {
    DiscoveredAuth auth;
    if (!DiscoverAuthFlow(server_url, std::string(), auth)) return false;
    if (scenario == "auth/resource-mismatch") {
        if (NormalizeAuthResource(auth.prm_resource) ==
            NormalizeAuthResource(server_url))
            return false;
        std::cout << "Auth resource rejected (" << scenario << ")" << std::endl;
        return true;
    }
    if (NormalizeAuthResource(auth.issuer) ==
        NormalizeAuthResource(auth.as_base))
        return false;
    std::cout << "Auth metadata rejected (" << scenario << ")" << std::endl;
    return true;
}

bool DriveManualCodeScenario(const std::string& server_url,
    const DiscoveredAuth& auth, const std::string& client_id,
    const std::string& client_secret, bool use_basic,
    const std::string& scope_text, std::string& token) {
    std::string verifier, location;
    if (!ManualAuthorize(auth, client_id, scope_text, verifier, location))
        return false;
    std::string code = QueryValue(location, "code");
    if (code.empty()) return false;
    std::map<std::string, std::string> fields = AuthCodeTokenFields(auth,
        client_id, code, verifier, scope_text);
    std::string basic;
    if (use_basic) {
        if (client_secret.empty()) return false;
        basic = Base64Encode(client_id + ":" + client_secret);
    } else if (!client_secret.empty()) {
        fields["client_secret"] = client_secret;
    }
    if (!ManualToken(auth, fields, basic, token)) return false;
    ListToolsWithBearer(server_url, token);
    return true;
}

bool DriveManualAuthScenario(const std::string& server_url,
    std::string_view scenario, const JsonValue& context) {
    DiscoveredAuth auth;
    if (!DiscoverAuthFlow(server_url, std::string(), auth)) return false;
    std::cout << "Auth discovery done (" << scenario << ")" << std::endl;
    std::string client_id, client_secret, token;
    if (scenario == "auth/metadata-var2" ||
        scenario == "auth/metadata-var3") {
        if (!ManualRegister(auth, {}, client_id, client_secret)) return false;
        return DriveManualCodeScenario(server_url, auth, client_id,
            std::string(), false, std::string(), token);
    }
    if (scenario == "auth/token-endpoint-auth-basic" ||
        scenario == "auth/token-endpoint-auth-post") {
        if (!ManualRegister(auth, {}, client_id, client_secret)) return false;
        if (client_secret.empty()) return false;
        return DriveManualCodeScenario(server_url, auth, client_id,
            client_secret, scenario == "auth/token-endpoint-auth-basic",
            std::string(), token);
    }
    if (scenario == "auth/pre-registration") {
        client_id = ContextString(context, "client_id");
        client_secret = ContextString(context, "client_secret");
        if (client_id.empty() || client_secret.empty()) return false;
        return DriveManualCodeScenario(server_url, auth, client_id,
            client_secret, true, std::string(), token);
    }
    if (scenario == "auth/offline-access-scope") {
        if (!ManualRegister(auth, {"authorization_code", "refresh_token"},
                client_id, client_secret))
            return false;
        return DriveManualCodeScenario(server_url, auth, client_id,
            std::string(), false, "mcp:basic offline_access", token);
    }
    if (scenario == "auth/client-credentials-basic") {
        client_id = ContextString(context, "client_id");
        client_secret = ContextString(context, "client_secret");
        if (client_id.empty() || client_secret.empty()) return false;
        std::map<std::string, std::string> fields;
        fields["grant_type"] = "client_credentials";
        if (!auth.prm_resource.empty()) fields["resource"] = auth.prm_resource;
        if (!ManualToken(auth, fields,
                Base64Encode(client_id + ":" + client_secret), token))
            return false;
        ListToolsWithBearer(server_url, token);
        return true;
    }
    if (scenario == "auth/enterprise-managed-authorization") {
        client_id = ContextString(context, "client_id");
        client_secret = ContextString(context, "client_secret");
        std::string id_token = ContextString(context, "idp_id_token");
        std::string idp_endpoint = ContextString(context, "idp_token_endpoint");
        if (client_id.empty() || client_secret.empty() || id_token.empty() ||
            idp_endpoint.empty())
            return false;
        std::map<std::string, std::string> exchange;
        exchange["grant_type"] = std::string(kTokenExchangeGrant);
        exchange["subject_token"] = id_token;
        exchange["subject_token_type"] = std::string(kIdTokenType);
        exchange["requested_token_type"] = std::string(kIdJagType);
        exchange["audience"] = auth.issuer;
        exchange["resource"] = server_url;
        std::string exchange_body;
        for (const auto& [key, value] : exchange) {
            if (!exchange_body.empty()) exchange_body.push_back('&');
            exchange_body += FormEncode(key) + "=" + FormEncode(value);
        }
        RawHttpResult exchange_response = ConformanceHttpPost(idp_endpoint,
            "application/x-www-form-urlencoded", exchange_body);
        JsonValue exchange_document;
        if (!ParseJsonBody(exchange_response, exchange_document)) return false;
        std::string jag = JsonStringField(exchange_document, "access_token");
        if (jag.empty()) return false;
        std::map<std::string, std::string> grant;
        grant["grant_type"] = std::string(kJwtBearerGrant);
        grant["assertion"] = jag;
        if (!auth.prm_resource.empty()) grant["resource"] = auth.prm_resource;
        if (!ManualToken(auth, grant,
                Base64Encode(client_id + ":" + client_secret), token))
            return false;
        ListToolsWithBearer(server_url, token);
        return true;
    }
    if (scenario == "auth/wif-jwt-bearer") {
        std::string assertion = ContextString(context, "valid_jwt");
        if (assertion.empty()) return false;
        std::map<std::string, std::string> fields;
        fields["grant_type"] = std::string(kJwtBearerGrant);
        fields["assertion"] = assertion;
        if (!auth.prm_resource.empty()) fields["resource"] = auth.prm_resource;
        if (!ManualToken(auth, fields, std::string(), token)) return false;
        ListToolsWithBearer(server_url, token);
        return true;
    }
    return false;
}

bool DriveClientCredentialsJwtScenario(const std::string& server_url,
    const JsonValue& context) {
    std::string client_id = ContextString(context, "client_id");
    std::string client_secret = ContextString(context, "client_secret");
    std::string signing_algorithm = ContextString(context, "signing_algorithm");
    std::string private_key_pem = ContextString(context, "private_key_pem");
    if (client_id.empty() || private_key_pem.empty()) return false;
    DiscoveredAuth auth;
    if (!DiscoverAuthFlow(server_url, std::string(), auth)) return false;
    std::cout << "Auth discovery done (auth/client-credentials-jwt)"
              << std::endl;
    OAuthClientOptions oauth_options;
    oauth_options.server_url = auth.as_base;
    if (!auth.prm_resource.empty()) oauth_options.resource = auth.prm_resource;
    oauth_options.redirect_uri = "http://localhost/callback";
    oauth_options.client_id = client_id;
    if (!client_secret.empty()) oauth_options.client_secret = client_secret;
    oauth_options.scopes = SelectAuthScopes(auth);
    oauth_options.signing_algorithm = signing_algorithm;
    oauth_options.private_key_pem = std::move(private_key_pem);
    auto provider = std::make_shared<OAuthClientProvider>(oauth_options);
    if (!provider->AuthenticateClientCredentials()) {
        std::cerr << "client_credentials private_key_jwt flow did not yield a "
                     "token"
                  << std::endl;
        return false;
    }
    std::string access_token = provider->GetAccessToken();
    if (access_token.empty()) {
        std::cerr << "client_credentials private_key_jwt flow stored no access "
                     "token"
                  << std::endl;
        return false;
    }
    auto client = ConnectBearerClient(server_url, "Bearer", access_token);
    try {
        auto tools = client->ListTools();
        std::cout << "client_credentials JWT tools listed ("
                  << tools.tools.size() << ")" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "client_credentials JWT bearer call rejected: " << e.what()
                  << std::endl;
        client->Close();
        return false;
    }
    client->Close();
    return true;
}

bool DriveDPoPScenario(const std::string& server_url,
    std::string_view scenario) {
    DiscoveredAuth auth;
    if (!DiscoverAuthFlow(server_url, std::string(), auth)) return false;
    std::cout << "Auth discovery done (" << scenario << ")" << std::endl;
    std::string client_id, client_secret;
    if (!ManualRegister(auth, {}, client_id, client_secret)) return false;
    auto token_cache = std::make_shared<InMemoryTokenCache>();
    OAuthClientOptions oauth_options;
    oauth_options.server_url = auth.as_base;
    if (!auth.prm_resource.empty()) oauth_options.resource = auth.prm_resource;
    oauth_options.redirect_uri = "http://localhost/callback";
    oauth_options.client_id = client_id;
    oauth_options.enable_dpop = true;
    oauth_options.token_cache = token_cache;
    auto provider = std::make_shared<OAuthClientProvider>(oauth_options);
    std::string scope_text;
    for (const auto& scope : SelectAuthScopes(auth)) {
        if (!scope_text.empty()) scope_text += " ";
        scope_text += scope;
    }
    std::string verifier, location;
    if (!ManualAuthorize(auth, client_id, scope_text, verifier, location))
        return false;
    std::string code = QueryValue(location, "code");
    if (code.empty()) return false;
    std::string token;
    if (!ManualDpopToken(auth, AuthCodeTokenFields(auth, client_id, code,
            verifier, scope_text), provider, token))
        return false;
    ResetDPoPNonce(provider);
    TokenContainer tokens;
    tokens.access_token = token;
    tokens.token_type = "DPoP";
    tokens.expires_at =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() +
        kDpopTokenLifetimeMs;
    token_cache->StoreTokens(tokens);
    auto client = ConnectDPoPClient(server_url, token, provider);
    try {
        auto tools = client->ListTools();
        std::cout << "DPoP tools listed (" << tools.tools.size() << ")"
                  << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "DPoP request rejected: " << e.what() << std::endl;
        client->Close();
        return false;
    }
    client->Close();
    return true;
}

bool DriveStepUpScenario(const std::string& server_url) {
    DiscoveredAuth auth;
    if (!DiscoverAuthFlow(server_url, std::string(), auth)) return false;
    std::string client_id, client_secret, token;
    if (!ManualRegister(auth, {}, client_id, client_secret)) return false;
    std::string verifier, location;
    if (!ManualAuthorize(auth, client_id, "mcp:basic", verifier, location))
        return false;
    std::string code = QueryValue(location, "code");
    if (code.empty()) return false;
    if (!ManualToken(auth, AuthCodeTokenFields(auth, client_id, code,
            verifier, "mcp:basic"), std::string(), token))
        return false;
    auto first_client = ConnectBearerClient(server_url, "Bearer", token);
    auto tools = first_client->ListTools();
    if (!FindTool(tools, "test-tool")) return false;
    try {
        first_client->CallTool("test-tool", JsonValue(JsonValue::Object{}));
    } catch (...) {
    }
    first_client->Close();
    std::string second_token;
    if (!ManualAuthorize(auth, client_id, "mcp:basic mcp:write", verifier,
            location))
        return false;
    code = QueryValue(location, "code");
    if (code.empty()) return false;
    if (!ManualToken(auth, AuthCodeTokenFields(auth, client_id, code,
            verifier, "mcp:basic mcp:write"), std::string(), second_token))
        return false;
    auto second_client =
        ConnectBearerClient(server_url, "Bearer", second_token);
    second_client->CallTool("test-tool", JsonValue(JsonValue::Object{}));
    second_client->Close();
    std::cout << "Auth step-up done" << std::endl;
    return true;
}

bool DriveMigrationScenario(const std::string& server_url) {
    DiscoveredAuth auth;
    if (!DiscoverAuthFlow(server_url, std::string(), auth)) return false;
    std::string first_as = auth.as_base;
    std::string client_id, client_secret, token;
    if (!ManualRegister(auth, {}, client_id, client_secret)) return false;
    std::string verifier, location;
    if (!ManualAuthorize(auth, client_id, "mcp:basic", verifier, location))
        return false;
    std::string code = QueryValue(location, "code");
    if (code.empty()) return false;
    if (!ManualToken(auth, AuthCodeTokenFields(auth, client_id, code,
            verifier, "mcp:basic"), std::string(), token))
        return false;
    auto first_client = ConnectBearerClient(server_url, "Bearer", token);
    first_client->Close();
    JsonValue prm;
    if (!ParseJsonBody(ConformanceHttpGet(auth.prm_url), prm)) return false;
    std::vector<std::string> servers =
        JsonStringList(prm, "authorization_servers");
    if (servers.empty()) return false;
    std::string second_as = servers.front();
    while (!second_as.empty() && second_as.back() == '/') second_as.pop_back();
    if (second_as == first_as) return false;
    DiscoveredAuth second_auth = auth;
    if (!FetchAsEndpoints(second_as, second_auth)) return false;
    std::string second_id, second_secret, second_token;
    if (!ManualRegister(second_auth, {}, second_id, second_secret))
        return false;
    if (!ManualAuthorize(second_auth, second_id, "mcp:basic", verifier,
            location))
        return false;
    code = QueryValue(location, "code");
    if (code.empty()) return false;
    if (!ManualToken(second_auth, AuthCodeTokenFields(second_auth, second_id,
            code, verifier, "mcp:basic"), std::string(), second_token))
        return false;
    auto second_client =
        ConnectBearerClient(server_url, "Bearer", second_token);
    if (!FindTool(second_client->ListTools(), "test-tool")) {
        second_client->Close();
        return false;
    }
    second_client->CallTool("test-tool", JsonValue(JsonValue::Object{}));
    second_client->Close();
    std::cout << "Auth migration done" << std::endl;
    return true;
}

bool DriveLegacyAuthScenario(const std::string& server_url,
    std::string_view scenario) {
    std::string origin = OriginOf(server_url);
    if (scenario == "auth/2025-03-26-oauth-metadata-backcompat") {
        DiscoveredAuth auth;
        if (!FetchAsEndpoints(origin, auth)) return false;
        auth.prm_resource = server_url;
        return RunProviderAuthFlow(server_url, auth, {}, std::string(), false,
            1);
    }
    RawHttpResult response = ConformanceHttpPost(origin + "/register",
        "application/json",
        "{\"redirect_uris\":[\"http://localhost/callback\"],"
        "\"client_name\":\"mcp-cpp-client\",\"application_type\":\"native\"}");
    JsonValue document;
    if (!ParseJsonBody(response, document)) return false;
    std::string client_id = JsonStringField(document, "client_id");
    if (client_id.empty()) return false;
    DiscoveredAuth auth;
    auth.as_base = origin;
    auth.prm_resource = server_url;
    return RunProviderAuthFlow(server_url, auth, {}, client_id, false, 1);
}

void RunAuthClient(const std::string& server_url, std::string_view scenario) {
    bool completed = false;
    if (scenario == "auth/metadata-default" ||
        scenario == "auth/metadata-var1" ||
        scenario == "auth/basic-cimd" ||
        scenario == "auth/scope-from-www-authenticate" ||
        scenario == "auth/scope-from-scopes-supported" ||
        scenario == "auth/scope-omitted-when-undefined" ||
        scenario == "auth/scope-step-up" ||
        scenario == "auth/scope-retry-limit") {
        if (scenario == "auth/scope-step-up")
            completed = DriveStepUpScenario(server_url);
        else
            completed = DriveProviderAuthScenario(server_url, scenario);
    } else if (scenario == "auth/token-endpoint-auth-basic" ||
        scenario == "auth/token-endpoint-auth-post" ||
        scenario == "auth/token-endpoint-auth-none" ||
        scenario == "auth/pre-registration") {
        if (scenario == "auth/token-endpoint-auth-none")
            completed = DriveProviderAuthScenario(server_url, scenario);
        else
            completed = DriveManualAuthScenario(server_url, scenario,
                ReadConformanceContext());
    } else if (scenario == "auth/metadata-var2" ||
        scenario == "auth/metadata-var3" ||
        scenario == "auth/offline-access-scope" ||
        scenario == "auth/client-credentials-basic" ||
        scenario == "auth/enterprise-managed-authorization" ||
        scenario == "auth/wif-jwt-bearer") {
        completed = DriveManualAuthScenario(server_url, scenario,
            ReadConformanceContext());
    } else if (scenario == "auth/offline-access-not-supported" ||
        scenario == "auth/iss-supported" ||
        scenario == "auth/iss-not-advertised" ||
        scenario == "auth/iss-supported-missing" ||
        scenario == "auth/iss-wrong-issuer" ||
        scenario == "auth/iss-unexpected" ||
        scenario == "auth/iss-normalized") {
        completed = DriveProviderAuthScenario(server_url, scenario);
    } else if (scenario == "auth/authorization-server-migration") {
        completed = DriveMigrationScenario(server_url);
    } else if (scenario == "auth/resource-mismatch" ||
        scenario == "auth/metadata-issuer-mismatch") {
        completed = DriveRejectScenario(server_url, scenario);
    } else if (scenario == "auth/2025-03-26-oauth-metadata-backcompat" ||
        scenario == "auth/2025-03-26-oauth-endpoint-fallback") {
        completed = DriveLegacyAuthScenario(server_url, scenario);
    } else if (scenario == "auth/client-credentials-jwt") {
        completed = DriveClientCredentialsJwtScenario(server_url,
            ReadConformanceContext());
    } else if (scenario == "auth/dpop" || scenario == "auth/dpop-nonce") {
        completed = DriveDPoPScenario(server_url, scenario);
    } else {
        std::cerr << "Unknown auth scenario: " << scenario << std::endl;
    }
    if (!completed)
        throw std::runtime_error(
            std::string("auth scenario did not complete: ") +
            std::string(scenario));
    std::cout << "Auth scenario done (" << scenario << ")" << std::endl;
}

using ScenarioHandler = void (*)(const std::string&);

const std::map<std::string, ScenarioHandler>& ScenarioRegistry() {
    static const std::map<std::string, ScenarioHandler> registry = {
        {"initialize", RunBasicClient},
        {"tools_call", RunToolsCallClient},
        {"request-metadata", RunRequestMetadataClient},
        {"http-standard-headers", RunHttpStandardHeadersClient},
        {"http-custom-headers", RunHttpCustomHeadersClient},
        {"http-invalid-tool-headers", RunHttpInvalidToolHeadersClient},
        {"sep-2322-client-request-state", RunMrtrClient},
        {"elicitation-sep1034-client-defaults", RunElicitationDefaultsClient},
        {"sse-retry", RunSSERetryClient},
        {"json-schema-ref-no-deref", RunJsonSchemaRefNoDerefClient},
        {"json-schema-2020-12-preservation",
            RunJsonSchema2020_12PreservationClient},
        {"sep-2640-client-no-prefetch", RunSkillsNoPrefetchClient},
        {"sep-2640-client-verify-digest", RunSkillsVerifyClient},
        {"sep-2640-client-verify-size", RunSkillsVerifyClient},
        {"sep-2640-client-verify-frontmatter", RunSkillsVerifyClient},
    };
    return registry;
}

void PrintUsageAndScenarios(std::ostream& out) {
    out << "Usage: MCP_CONFORMANCE_SCENARIO=<scenario> conformance-client "
           "<server-url>" << std::endl;
    out << "\nThe MCP_CONFORMANCE_SCENARIO env var is set automatically by "
           "the conformance runner." << std::endl;
    out << "\nAvailable scenarios:" << std::endl;
    for (const auto& [name, handler] : ScenarioRegistry())
        out << "  - " << name << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    const char* scenario_name = std::getenv("MCP_CONFORMANCE_SCENARIO");
    if (!scenario_name || argc < 2) {
        PrintUsageAndScenarios(std::cerr);
        return 1;
    }

    std::string_view scenario_view(scenario_name);
    if (IsAuthScenario(scenario_view)) {
        try {
            RunAuthClient(argv[1], scenario_view);
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }
    }

    const auto& registry = ScenarioRegistry();
    auto it = registry.find(scenario_name);
    if (it == registry.end()) {
        std::cerr << "Unknown scenario: " << scenario_name << std::endl;
        PrintUsageAndScenarios(std::cerr);
        return 1;
    }

    try {
        it->second(argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
