// feeds-json-lite.h — minimal, dependency-free JSON helpers for IPC payloads.
//
// These are the hand-rolled string helpers the plugin uses to read engine→plugin
// messages (participant rosters, chat, Zoom Events) and to escape outgoing
// values. They are intentionally NOT a general JSON parser — just enough to pull
// named scalar/string fields and to split the object arrays the engine emits,
// with string-awareness so a display name containing '{', '}', '[', ']' or an
// escaped quote can't desync the array/object splitting (the v1.5.1 fix).
//
// Hoisted out of plugin-main.cpp so the same code the plugin ships can be unit-
// tested in isolation (tests/test_e2p_reader.cpp) without libobs/Qt. Pure
// std::string; header-only via `inline`. The engine keeps its own mirror copies
// in engine-api.cpp — migrating those to this header is a worthwhile follow-up,
// but out of scope here to keep the change bounded.
#pragma once

#include <string>
#include <vector>
#include <cstdio>

namespace feeds {

inline std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + search.size() + 1);
    if (pos == std::string::npos) return "";
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

inline long long ExtractJsonNumber(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos = json.find_first_of("-0123456789", pos + search.size());
    if (pos == std::string::npos) return 0;
    size_t end = json.find_first_not_of("-0123456789", pos);
    std::string numStr = json.substr(pos, end == std::string::npos
                                          ? std::string::npos : end - pos);
    try { return std::stoll(numStr); } catch (...) { return 0; }
}

// JSON string escaper for outgoing IPC. Mirrors the engine's JsonEscape
// — they need to agree on what counts as an escape so both sides can
// round-trip user-entered chat content (quotes, backslashes, newlines).
inline std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// JSON-escape-aware string extractor. ExtractJsonString stops at the first
// quote without handling escapes, which truncates chat content containing
// quotes — extremely common in actual chat traffic. This walks the value
// and decodes the escape sequences emitted by the engine's JsonEscape: \",
// \\, \/, \b, \f, \n, \r, \t, and \uXXXX. \uXXXX is dropped silently —
// engine only emits it for control chars below 0x20, which aren't visible
// in QListWidget items anyway.
inline std::string ExtractJsonStringEscaped(const std::string& json,
                                            const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();

    std::string out;
    out.reserve(json.size() - pos);
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') break;
        if (c == '\\' && pos + 1 < json.size()) {
            char n = json[pos + 1];
            switch (n) {
                case '"': case '\\': case '/': out += n;   pos += 2; break;
                case 'b': out += '\b'; pos += 2; break;
                case 'f': out += '\f'; pos += 2; break;
                case 'n': out += '\n'; pos += 2; break;
                case 'r': out += '\r'; pos += 2; break;
                case 't': out += '\t'; pos += 2; break;
                case 'u': pos += 6; break;
                default:  out += n; pos += 2; break;
            }
        } else {
            out += c;
            pos += 1;
        }
    }
    return out;
}

// Return the text of the array value for `key` (from its '[' to the matching
// ']'), string- and bracket-aware. Used to pull the events[]/sessions[] arrays
// out of the Zoom Events IPC payloads. Mirrors the engine's JsonExtractArrayBody.
inline std::string JsonExtractArrayBody(const std::string& json,
                                        const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find('[', pos + search.size());
    if (pos == std::string::npos) return "";

    int depth = 0; bool inStr = false; bool esc = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '[') depth++;
        else if (c == ']') { if (--depth == 0) return json.substr(pos, i - pos + 1); }
    }
    return "";
}

// Split a JSON array body into its top-level object substrings ("{...}"),
// skipping nested braces and braces inside strings.
inline std::vector<std::string> SplitJsonObjects(const std::string& arr) {
    std::vector<std::string> out;
    int depth = 0; bool inStr = false; bool esc = false; size_t start = 0;
    for (size_t i = 0; i < arr.size(); ++i) {
        char c = arr[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}') { if (--depth == 0) out.push_back(arr.substr(start, i - start + 1)); }
    }
    return out;
}

} // namespace feeds
