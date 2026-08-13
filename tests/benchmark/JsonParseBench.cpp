// JsonParseBench.cpp — JSON parse/dump throughput benchmark on mcp-core

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include <mcp/JsonValue.hpp>

using namespace mcp;

namespace {

struct BenchCase {
    const char* name;
    const char* json;
};

const BenchCase kCases[] = {
    {
        "tools/call",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}}})",
    },
    {
        "tools/list",
        R"({"jsonrpc":"2.0","id":1,"result":{"tools":[
{"name":"get_weather","description":"Get current weather conditions for a city including temperature, humidity, wind speed, precipitation chance, and a human readable forecast summary","inputSchema":{"type":"object","properties":{"city":{"type":"string"},"units":{"type":"string"}}}},
{"name":"get_news","description":"Fetch the latest news headlines for a topic, optionally filtered by source, language, and recency, returning title, publisher, and publish timestamp for each story","inputSchema":{"type":"object","properties":{"topic":{"type":"string"},"limit":{"type":"integer"}}}},
{"name":"get_stock","description":"Get the current price, day change, trading volume, and 52 week high and low for a stock symbol from the market data feed","inputSchema":{"type":"object","properties":{"symbol":{"type":"string"}}}},
{"name":"search_files","description":"Search for files and directories matching a glob query under a directory, returning relative paths with file sizes and last modified times","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"dir":{"type":"string"}}}},
{"name":"read_file","description":"Read the full contents of a text file, returning the content along with its size in bytes and the detected encoding","inputSchema":{"type":"object","properties":{"path":{"type":"string"}}}},
{"name":"write_file","description":"Write content to a file, creating parent directories as needed and returning whether an existing file was overwritten","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}}}},
{"name":"list_dir","description":"List the entries of a directory, returning each entry name, type (file or directory), and size, sorted alphabetically","inputSchema":{"type":"object","properties":{"path":{"type":"string"}}}},
{"name":"delete_file","description":"Permanently delete a file, moving it to the trash first when the trash option is enabled to allow recovery","inputSchema":{"type":"object","properties":{"path":{"type":"string"}}}},
{"name":"send_email","description":"Send an email message through the configured SMTP relay, supporting plain text and HTML bodies and up to ten attachments","inputSchema":{"type":"object","properties":{"to":{"type":"string"},"subject":{"type":"string"},"body":{"type":"string"}}}},
{"name":"create_calendar_event","description":"Create a calendar event with title, start and end times, optional location and attendees, and a reminder offset in minutes","inputSchema":{"type":"object","properties":{"title":{"type":"string"},"start_time":{"type":"string"}}}},
{"name":"list_calendar_events","description":"List calendar events for a given date, returning each event id, title, start time, duration, and attendee count","inputSchema":{"type":"object","properties":{"date":{"type":"string"}}}},
{"name":"set_reminder","description":"Set a reminder that fires at a given time, optionally repeating daily, weekly, or monthly, and returning the reminder id","inputSchema":{"type":"object","properties":{"text":{"type":"string"},"time":{"type":"string"}}}},
{"name":"translate_text","description":"Translate text from its detected source language into a target language, preserving formatting and returning the confidence score","inputSchema":{"type":"object","properties":{"text":{"type":"string"},"target_lang":{"type":"string"}}}},
{"name":"summarize_text","description":"Summarize a piece of text into a concise summary of up to max words, keeping key facts and the original tone","inputSchema":{"type":"object","properties":{"text":{"type":"string"},"max_words":{"type":"integer"}}}},
{"name":"detect_language","description":"Detect the language of a text sample, returning the language code, the full language name, and a confidence percentage","inputSchema":{"type":"object","properties":{"text":{"type":"string"}}}},
{"name":"get_timezone","description":"Get the IANA timezone identifier, current UTC offset, and daylight saving rules for a city or region","inputSchema":{"type":"object","properties":{"location":{"type":"string"}}}},
{"name":"convert_units","description":"Convert a numeric value from one unit to another for length, mass, temperature, volume, and time categories, returning the converted value","inputSchema":{"type":"object","properties":{"value":{"type":"number"},"from":{"type":"string"},"to":{"type":"string"}}}},
{"name":"calculate","description":"Evaluate a math expression supporting arithmetic, parentheses, exponents, trigonometry, logarithms, and constants like pi and e","inputSchema":{"type":"object","properties":{"expression":{"type":"string"}}}},
{"name":"fetch_url","description":"Fetch a URL over HTTP or HTTPS and return the response body, status code, and content type, with an optional timeout in seconds","inputSchema":{"type":"object","properties":{"url":{"type":"string"},"timeout":{"type":"integer"}}}},
{"name":"query_database","description":"Run a read-only SQL query against the connected database, returning the column names and up to one thousand result rows","inputSchema":{"type":"object","properties":{"sql":{"type":"string"}}}}
]}})",
    },
    {
        "initialize",
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2026-07-28","capabilities":{},"clientInfo":{"name":"bench","version":"1.0"}}})",
    },
    {
        "error",
        R"({"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"invalid params","data":{"field":"name"}}})",
    },
    {
        "deep-nested",
        R"({"a":{"b":{"c":["s1",{"d":{"e":{"f":[1,2.5,{"g":{"h":{"i":["x",{"j":{"k":{"l":{"m":true}}}}]}}}]}}}]}}})",
    },
};

constexpr int kWarmupIterations = 200;
constexpr int kMeasureIterations = 50000;

void RunBench(const BenchCase& c) {
    const std::string_view json(c.json);
    const std::size_t bytes = json.size();

    for (int i = 0; i < kWarmupIterations; ++i) {
        static_cast<void>(JsonValue::Parse(json));
    }

    const auto parseStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kMeasureIterations; ++i) {
        static_cast<void>(JsonValue::Parse(json));
    }
    const auto parseEnd = std::chrono::steady_clock::now();

    const JsonValue parsed = JsonValue::Parse(json);
    const auto dumpStart = std::chrono::steady_clock::now();
    static_cast<void>(parsed.Dump());
    const auto dumpEnd = std::chrono::steady_clock::now();

    const double parseNs = std::chrono::duration_cast<std::chrono::nanoseconds>(parseEnd - parseStart).count() / static_cast<double>(kMeasureIterations);
    const double dumpNs = std::chrono::duration_cast<std::chrono::nanoseconds>(dumpEnd - dumpStart).count();
    const double parseMBps = static_cast<double>(bytes) / (parseNs * 1e-9) / 1e6;
    const double dumpMBps = static_cast<double>(bytes) / (dumpNs * 1e-9) / 1e6;
    std::printf("%s: Parse %.2f MB/s (%.2f ns/op), Dump %.2f MB/s\n", c.name, parseMBps, parseNs, dumpMBps);
}

} // namespace

int main() {
    for (const BenchCase& c : kCases) {
        RunBench(c);
    }
    return 0;
}
