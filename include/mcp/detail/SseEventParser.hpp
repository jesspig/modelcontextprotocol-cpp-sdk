#pragma once

#include <mcp/detail/HttpFraming.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp { namespace detail {

template <typename Fn>
inline void ForEachSseLine(std::string_view block, Fn&& fn) {
    std::size_t pos = 0;
    while (pos < block.size()) {
        std::size_t end = block.find('\n', pos);
        std::string_view line = (end == std::string_view::npos)
            ? block.substr(pos)
            : block.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        fn(line);
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
}

struct SseFieldLine {
    std::string_view name;
    std::string_view value;
    bool has_payload = false;
};

[[nodiscard]] inline bool ParseSseFieldLine(std::string_view line, SseFieldLine& out) {
    if (line.empty() || line.front() == ':') return false;
    std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        out.name = line;
        out.value = std::string_view{};
        out.has_payload = false;
        return true;
    }
    if (colon == 0) return false;
    out.name = line.substr(0, colon);
    std::string_view rest = line.substr(colon + 1);
    std::size_t first = rest.find_first_not_of(" \t");
    out.value = (first == std::string_view::npos) ? std::string_view{} : rest.substr(first);
    out.has_payload = !rest.empty();
    return true;
}

inline void AppendSseData(std::string& data, std::string_view value) {
    if (!data.empty()) data += '\n';
    data += value;
}

struct SseEvent {
    std::string event_type = "message";
    std::string data;
    std::string id;
    std::optional<uint64_t> retry_ms;
};

struct SseFeedResult {
    std::vector<SseEvent> events;
    bool limit_exceeded = false;
};

class SseIncrementalParser {
public:
    explicit SseIncrementalParser(std::size_t max_event_bytes)
        : limit_(max_event_bytes) {}

    [[nodiscard]] SseFeedResult Feed(std::string_view bytes) {
        SseFeedResult out;
        for (char c : bytes) {
            if (pending_cr_) {
                pending_cr_ = false;
                if (c == '\n') {
                    CompleteLine(out);
                    if (out.limit_exceeded) return out;
                    continue;
                }
                CompleteLine(out);
                if (out.limit_exceeded) return out;
            }
            if (c == '\r') {
                pending_cr_ = true;
                CountByte(out);
                if (out.limit_exceeded) return out;
                continue;
            }
            if (c == '\n') {
                CompleteLine(out);
                if (out.limit_exceeded) return out;
                continue;
            }
            line_.push_back(c);
            CountByte(out);
            if (out.limit_exceeded) return out;
        }
        return out;
    }

    [[nodiscard]] SseFeedResult Flush(bool include_incomplete_line = false) {
        SseFeedResult out;
        if (pending_cr_) {
            pending_cr_ = false;
            CompleteLine(out);
        } else if (!line_.empty()) {
            if (include_incomplete_line) CompleteLine(out);
            else line_.clear();
        }
        if (out.limit_exceeded) return out;
        Dispatch(out);
        return out;
    }

private:
    void CountByte(SseFeedResult& out) {
        if (event_bytes_ == (std::numeric_limits<std::size_t>::max)()) {
            out.limit_exceeded = true;
            return;
        }
        ++event_bytes_;
        if (event_bytes_ > limit_) out.limit_exceeded = true;
    }

    void CompleteLine(SseFeedResult& out) {
        const bool empty_line = line_.empty();
        SseFieldLine field;
        if (ParseSseFieldLine(line_, field)) {
            if (field.name == "data") {
                AppendSseData(event_.data, field.value);
            } else if (field.name == "id") {
                event_.id = std::string(field.value);
            } else if (field.name == "event") {
                event_.event_type = std::string(field.value);
            } else if (field.name == "retry") {
                uint64_t value = 0;
                if (ParseCheckedUint64(field.value, value)) event_.retry_ms = value;
            }
        }
        line_.clear();
        if (empty_line) Dispatch(out);
    }

    void Dispatch(SseFeedResult& out) {
        if (!event_.data.empty() || !event_.id.empty() || event_.retry_ms.has_value() ||
            event_.event_type != "message") {
            out.events.push_back(std::move(event_));
        }
        event_ = SseEvent{};
        event_bytes_ = 0;
    }

    std::size_t limit_ = (std::numeric_limits<std::size_t>::max)();
    std::size_t event_bytes_ = 0;
    std::string line_;
    bool pending_cr_ = false;
    SseEvent event_;
};

}} // namespace mcp::detail
