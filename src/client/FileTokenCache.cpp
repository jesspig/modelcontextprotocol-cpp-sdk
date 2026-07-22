#include <mcp/JsonValue.hpp>
#include <mcp/storage/FileTokenCache.hpp>
#include <mcp/Log.hpp>

#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#else
#include <sys/stat.h>
#endif

namespace {

#ifdef _WIN32
std::vector<BYTE> ProtectData(const std::string& data) {
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(data.data()));
    input.cbData = static_cast<DWORD>(data.size());
    DATA_BLOB output;
    if (CryptProtectData(&input, L"MCP Token Cache", nullptr, nullptr, nullptr, 0, &output)) {
        std::vector<BYTE> result(output.pbData, output.pbData + output.cbData);
        LocalFree(output.pbData);
        return result;
    }
    return {};
}

std::string UnprotectData(const std::vector<BYTE>& data) {
    DATA_BLOB input;
    input.pbData = const_cast<BYTE*>(data.data());
    input.cbData = static_cast<DWORD>(data.size());
    DATA_BLOB output;
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);
        return result;
    }
    return {};
}
#endif

} // namespace

namespace mcp {

FileTokenCache::FileTokenCache(std::filesystem::path cache_path)
    : cache_path_(std::move(cache_path))
{
    Load();
}

FileTokenCache::~FileTokenCache() {
    Save();
}

void FileTokenCache::StoreTokens(const TokenContainer& tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_ = tokens;
    Save();
}

std::optional<TokenContainer> FileTokenCache::GetTokens() {
    std::lock_guard<std::mutex> lock(mutex_);
    return tokens_;
}

void FileTokenCache::ClearTokens() {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_.reset();
    Save();
}

void FileTokenCache::Load() {
    if (!std::filesystem::exists(cache_path_)) return;

#ifdef _WIN32
    std::ifstream file(cache_path_, std::ios::binary);
    if (!file.is_open()) return;
    std::vector<BYTE> buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    auto json_str = UnprotectData(buf);

    if (json_str.empty()) {
        // Fallback: try plaintext JSON (migration from unencrypted cache)
        std::ifstream plain_file(cache_path_);
        if (!plain_file.is_open()) return;
        try {
            std::string content((std::istreambuf_iterator<char>(plain_file)), std::istreambuf_iterator<char>());
            auto json = JsonValue::Parse(content);
            if (json.IsNull()) return;
            TokenContainer tc;
            if (auto* v = json.Find("access_token")) tc.access_token = v->GetString();
            if (auto* v = json.Find("refresh_token")) tc.refresh_token = v->GetString();
            if (auto* v = json.Find("token_type")) tc.token_type = v->GetString();
            if (auto* v = json.Find("expires_at")) tc.expires_at = v->GetInt();
            if (auto* v = json.Find("scopes"); v && v->IsArray()) {
                for (const auto& s : v->GetArray())
                    tc.scopes.push_back(s.GetString());
            }
            tokens_ = std::move(tc);
        } catch (...) { MCP_LOG(Warning, "token cache fallback parse failed"); }
        return;
    }

    try {
        auto json = JsonValue::Parse(json_str);
        if (json.IsNull()) return;
        TokenContainer tc;
        if (auto* v = json.Find("access_token")) tc.access_token = v->GetString();
        if (auto* v = json.Find("refresh_token")) tc.refresh_token = v->GetString();
        if (auto* v = json.Find("token_type")) tc.token_type = v->GetString();
        if (auto* v = json.Find("expires_at")) tc.expires_at = v->GetInt();
        if (auto* v = json.Find("scopes"); v && v->IsArray()) {
            for (const auto& s : v->GetArray())
                tc.scopes.push_back(s.GetString());
        }
        tokens_ = std::move(tc);
    } catch (...) {
        MCP_LOG(Warning, "token cache parse failed (Win32)");
    }
#else
    std::ifstream file(cache_path_);
    if (!file.is_open()) return;
    try {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto json = JsonValue::Parse(content);
        if (json.IsNull()) return;
        TokenContainer tc;
        if (auto* v = json.Find("access_token")) tc.access_token = v->GetString();
        if (auto* v = json.Find("refresh_token")) tc.refresh_token = v->GetString();
        if (auto* v = json.Find("token_type")) tc.token_type = v->GetString();
        if (auto* v = json.Find("expires_at")) tc.expires_at = v->GetInt();
        if (auto* v = json.Find("scopes"); v && v->IsArray()) {
            for (const auto& s : v->GetArray())
                tc.scopes.push_back(s.GetString());
        }
        tokens_ = std::move(tc);
    } catch (...) {
        MCP_LOG(Warning, "token cache parse failed");
    }
    file.close();
    chmod(cache_path_.string().c_str(), 0600);
#endif
}

void FileTokenCache::Save() {
    if (!tokens_) {
        std::filesystem::remove(cache_path_);
        return;
    }
    JsonValue::Object obj;
    obj["access_token"] = JsonValue(tokens_->access_token);
    obj["refresh_token"] = JsonValue(tokens_->refresh_token);
    obj["token_type"] = JsonValue(tokens_->token_type);
    obj["expires_at"] = JsonValue(static_cast<int64_t>(tokens_->expires_at));
    {
        JsonValue::Array scopes_arr;
        for (const auto& s : tokens_->scopes)
            scopes_arr.push_back(JsonValue(s));
        obj["scopes"] = JsonValue(std::move(scopes_arr));
    }
    auto dump_str = JsonValue(std::move(obj)).Dump(2);
    auto tmp_path = cache_path_;
    tmp_path += ".tmp";
#ifdef _WIN32
    auto plaintext = dump_str;
    auto encrypted = ProtectData(plaintext);
    if (encrypted.empty()) {
        std::filesystem::remove(tmp_path);
        return;
    }
    {
        std::ofstream file(tmp_path, std::ios::binary);
        if (!file.is_open()) {
            std::filesystem::remove(tmp_path);
            return;
        }
        file.write(reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
    }
    std::filesystem::rename(tmp_path, cache_path_);
#else
    {
        std::ofstream file(tmp_path);
        if (!file.is_open()) {
            std::filesystem::remove(tmp_path);
            return;
        }
        file << dump_str;
        file.close();
        chmod(tmp_path.string().c_str(), 0600);
    }
    std::filesystem::rename(tmp_path, cache_path_);
#endif
}

} // namespace mcp
