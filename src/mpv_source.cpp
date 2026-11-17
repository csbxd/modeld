#include "modeld/input.hpp"
#include <charconv>
#include <httplib.h>
#include <map>
#include <string_view>

namespace modeld {
namespace {
using Fields = std::map<std::string, std::string>;
struct Entry {
    std::string first;
    Fields fields;
};
std::vector<Entry> parse_edl(std::string_view text) {
    std::vector<Entry> entries;
    size_t cursor = 0;
    auto invalid = [&] {
        log_event("source.timeline.invalid", {{"cursor", cursor}, {"bytes", text.size()}});
        throw Error(422, "Cannot read mpv's resolved video timeline");
    };
    while (cursor < text.size()) {
        if (text[cursor] == ';' || text[cursor] == '\n') {
            cursor++;
            continue;
        }
        Entry entry;
        size_t field = 0;
        do {
            std::string name, value;
            const auto start = cursor;
            if (text[cursor] != '%') {
                auto delimiter = text.find_first_of("=,;\n", cursor);
                if (delimiter != std::string_view::npos && text[delimiter] == '=' &&
                    text.substr(cursor, delimiter - cursor)
                            .find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXY"
                                               "Z0123456789_") == std::string_view::npos) {
                    name = text.substr(cursor, delimiter - cursor);
                    cursor = delimiter + 1;
                }
            }
            if (cursor < text.size() && text[cursor] == '%') {
                const auto end = text.find('%', cursor + 1);
                size_t length = 0;
                if (end == std::string_view::npos)
                    invalid();
                auto result = std::from_chars(text.data() + cursor + 1, text.data() + end, length);
                if (result.ec != std::errc{} || result.ptr != text.data() + end ||
                    length > text.size() - end - 1)
                    invalid();
                cursor = end + 1;
                value = text.substr(cursor, length);
                cursor += length;
                if (cursor < text.size() && text[cursor] != ',' && text[cursor] != ';' &&
                    text[cursor] != '\n')
                    invalid();
            } else {
                auto end = text.find_first_of(",;\n", cursor);
                if (end == std::string_view::npos)
                    end = text.size();
                value = text.substr(cursor, end - cursor);
                cursor = end;
            }
            if (field == 0 && name.empty())
                entry.first = std::move(value);
            else {
                if (name.empty())
                    name = field == 1 ? "start" : field == 2 ? "length" : "extra";
                entry.fields[name] = std::move(value);
            }
            field++;
            if (cursor == start)
                invalid();
            if (cursor >= text.size() || text[cursor] != ',')
                break;
            cursor++;
            if (cursor == text.size())
                invalid();
        } while (cursor < text.size());
        entries.push_back(std::move(entry));
    }
    return entries;
}
bool http(const std::string &url) {
    return url.starts_with("http://") || url.starts_with("https://");
}
} // namespace
std::string resolved_track_url(const std::string &opened, const Json &selected,
                               const std::string &kind) {
    if (http(opened))
        return opened;
    if (!opened.starts_with("edl://"))
        throw Error(422, "The selected network video has no resolved HTTP media URL");
    if (opened.size() > 4 * 1024 * 1024)
        throw Error(422, "The resolved video timeline is too large");
    struct Stream {
        std::string url;
        int64_t program = -1;
        int segments = 0;
        bool video = false, typed = false, no_clip = false, unsupported = false;
        std::string type;
    } current;
    std::vector<Stream> streams;
    auto finish = [&] {
        if (current.segments > 0)
            streams.push_back(std::move(current));
        current = {};
    };
    for (const auto &entry : parse_edl(std::string_view(opened).substr(6))) {
        if (entry.first == "!new_stream")
            finish();
        else if (entry.first == "!no_clip")
            current.no_clip = true;
        else if (entry.first == "!delay_open") {
            auto type = entry.fields.find("media_type");
            if (type != entry.fields.end()) {
                current.typed = true;
                current.video |= type->second == "video";
                current.type = type->second;
            }
        } else if (entry.first == "!track_meta") {
            auto program = entry.fields.find("program_id");
            if (program != entry.fields.end()) {
                auto &value = program->second;
                auto result =
                    std::from_chars(value.data(), value.data() + value.size(), current.program);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
                    current.unsupported = true;
            }
        } else if (entry.first == "!mp4_dash")
            current.unsupported = true;
        else if (!entry.first.empty() && entry.first.front() != '!') {
            current.segments++;
            current.url = entry.first;
            for (const auto &[name, value] : entry.fields)
                if ((name == "start" && value != "0" && value != "0.0") ||
                    (name == "length" && !current.no_clip))
                    current.unsupported = true;
        }
    }
    finish();
    Json summary = Json::array();
    for (const auto &stream : streams) {
        if (summary.size() == 32)
            break;
        summary.push_back({{"program", stream.program},
                           {"segments", stream.segments},
                           {"video", stream.video},
                           {"typed", stream.typed},
                           {"unsupported", stream.unsupported},
                           {"url_bytes", stream.url.size()},
                           {"http", http(stream.url)}});
    }
    log_event("source.timeline", {{"bytes", opened.size()},
                                  {"stream_count", streams.size()},
                                  {"streams", summary},
                                  {"selected", log_tracks(Json::array({selected}))}});
    std::vector<const Stream *> candidates;
    for (const auto &stream : streams)
        if (stream.type == kind || !stream.typed)
            candidates.push_back(&stream);
    const Stream *chosen = nullptr;
    if (selected.contains("program-id")) {
        const auto program = selected.at("program-id").get<int64_t>();
        for (const auto *stream : candidates)
            if (stream->program == program) {
                if (chosen)
                    throw Error(422, "The selected video maps to several resolved streams");
                chosen = stream;
            }
    }
    if (!chosen && candidates.size() == 1)
        chosen = candidates.front();
    if (!chosen && kind == "audio" && selected.contains("id")) {
        const auto index = selected.at("id").get<int>() - 1;
        if (index >= 0 && static_cast<size_t>(index) < candidates.size())
            chosen = candidates[index];
    }
    const std::string reason = !chosen                 ? "selected_stream_not_unique"
                               : chosen->segments != 1 ? "multiple_segments"
                               : chosen->unsupported   ? "fragmented_or_clipped_stream"
                               : !http(chosen->url)    ? "non_http_stream"
                                                       : "";
    if (!reason.empty()) {
        log_event("source.timeline.rejected",
                  {{"reason", reason}, {"candidates", candidates.size()}});
        throw Error(422, "Unsupported mpv video timeline: " + reason);
    }
    return chosen->url;
}
} // namespace modeld
