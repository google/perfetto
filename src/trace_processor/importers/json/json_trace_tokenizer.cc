/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/json/json_trace_tokenizer.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"
#include "src/trace_processor/sorter/trace_sorter.h"  // IWYU pragma: keep
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {
namespace {

std::string FormatErrorContext(const char* s, const char* e) {
  return {s, static_cast<size_t>(e - s)};
}

base::Status AppendUnescapedCharacter(char c,
                                      bool is_escaping,
                                      std::string* key) {
  if (is_escaping) {
    switch (c) {
      case '"':
      case '\\':
      case '/':
        key->push_back(c);
        break;
      case 'b':
        key->push_back('\b');
        break;
      case 'f':
        key->push_back('\f');
        break;
      case 'n':
        key->push_back('\n');
        break;
      case 'r':
        key->push_back('\r');
        break;
      case 't':
        key->push_back('\t');
        break;
      case 'u':
        // Just pass through \uxxxx escape sequences which JSON supports but is
        // not worth the effort to parse as we never use them here.
        key->append("\\u");
        break;
      default:
        return base::ErrStatus("Illegal character in JSON %c", c);
    }
  } else if (c != '\\') {
    key->push_back(c);
  }
  return base::OkStatus();
}

enum class ReadStringRes {
  kEndOfString,
  kNeedsMoreData,
  kFatalError,
};
ReadStringRes ReadOneJsonString(const char* start,
                                const char* end,
                                std::string* key,
                                const char** next) {
  if (start == end) {
    return ReadStringRes::kNeedsMoreData;
  }
  if (*start != '"') {
    return ReadStringRes::kFatalError;
  }

  bool is_escaping = false;
  for (const char* s = start + 1; s < end; s++) {
    // Control characters are not allowed in JSON strings.
    if (iscntrl(*s))
      return ReadStringRes::kFatalError;

    // If we get a quote character end of the string.
    if (*s == '"' && !is_escaping) {
      *next = s + 1;
      return ReadStringRes::kEndOfString;
    }

    base::Status status = AppendUnescapedCharacter(*s, is_escaping, key);
    if (!status.ok())
      return ReadStringRes::kFatalError;

    // If we're in a string and we see a backslash and the last character was
    // not a backslash the next character is escaped:
    is_escaping = *s == '\\' && !is_escaping;
  }
  return ReadStringRes::kNeedsMoreData;
}

enum class SkipValueRes {
  kEndOfValue,
  kNeedsMoreData,
  kFatalError,
};
SkipValueRes SkipOneJsonValue(const char* start,
                              const char* end,
                              const char** next) {
  uint32_t brace_count = 0;
  uint32_t bracket_count = 0;
  for (const char* s = start; s < end; s++) {
    if (*s == '"') {
      // Because strings can contain {}[] characters, handle them separately
      // before anything else.
      std::string ignored;
      const char* str_next = nullptr;
      switch (ReadOneJsonString(s, end, &ignored, &str_next)) {
        case ReadStringRes::kFatalError:
          return SkipValueRes::kFatalError;
        case ReadStringRes::kNeedsMoreData:
          return SkipValueRes::kNeedsMoreData;
        case ReadStringRes::kEndOfString:
          // -1 as the loop body will +1 getting to the correct place.
          s = str_next - 1;
          break;
      }
      continue;
    }
    if (brace_count == 0 && bracket_count == 0 && (*s == ',' || *s == '}')) {
      // Regardless of a comma or brace, this will be skipped by the caller so
      // just set it to this character.
      *next = s;
      return SkipValueRes::kEndOfValue;
    }
    if (*s == '[') {
      ++bracket_count;
      continue;
    }
    if (*s == ']') {
      if (bracket_count == 0) {
        return SkipValueRes::kFatalError;
      }
      --bracket_count;
      continue;
    }
    if (*s == '{') {
      ++brace_count;
      continue;
    }
    if (*s == '}') {
      if (brace_count == 0) {
        return SkipValueRes::kFatalError;
      }
      --brace_count;
      continue;
    }
  }
  return SkipValueRes::kNeedsMoreData;
}

base::Status SetOutAndReturn(const char* ptr, const char** out) {
  *out = ptr;
  return base::OkStatus();
}

}  // namespace

ReadDictRes ReadOneJsonDict(const char* start,
                            const char* end,
                            base::StringView* value,
                            const char** next) {
  int braces = 0;
  int square_brackets = 0;
  const char* dict_begin = nullptr;
  bool in_string = false;
  bool is_escaping = false;
  for (const char* s = start; s < end; s++) {
    if (isspace(*s) || *s == ',')
      continue;
    if (*s == '"' && !is_escaping) {
      in_string = !in_string;
      continue;
    }
    if (in_string) {
      // If we're in a string and we see a backslash and the last character was
      // not a backslash the next character is escaped:
      is_escaping = *s == '\\' && !is_escaping;
      // If we're currently parsing a string we should ignore otherwise special
      // characters:
      continue;
    }
    if (*s == '{') {
      if (braces == 0)
        dict_begin = s;
      braces++;
      continue;
    }
    if (*s == '}') {
      if (braces <= 0)
        return ReadDictRes::kEndOfTrace;
      if (--braces > 0)
        continue;
      auto len = static_cast<size_t>((s + 1) - dict_begin);
      *value = base::StringView(dict_begin, len);
      *next = s + 1;
      return ReadDictRes::kFoundDict;
    }
    if (*s == '[') {
      square_brackets++;
      continue;
    }
    if (*s == ']') {
      if (square_brackets == 0) {
        // We've reached the end of [traceEvents] array.
        // There might be other top level keys in the json (e.g. metadata)
        // after.
        *next = s + 1;
        return ReadDictRes::kEndOfArray;
      }
      square_brackets--;
    }
  }
  return ReadDictRes::kNeedsMoreData;
}

ReadKeyRes ReadOneJsonKey(const char* start,
                          const char* end,
                          std::string* key,
                          const char** next) {
  enum class NextToken {
    kStringOrEndOfDict,
    kColon,
    kValue,
  };

  NextToken next_token = NextToken::kStringOrEndOfDict;
  for (const char* s = start; s < end; s++) {
    // Whitespace characters anywhere can be skipped.
    if (isspace(*s))
      continue;

    switch (next_token) {
      case NextToken::kStringOrEndOfDict: {
        // If we see a closing brace, that means we've reached the end of the
        // wrapping dictionary.
        if (*s == '}') {
          *next = s + 1;
          return ReadKeyRes::kEndOfDictionary;
        }

        // If we see a comma separator, just ignore it.
        if (*s == ',')
          continue;

        auto res = ReadOneJsonString(s, end, key, &s);
        if (res == ReadStringRes::kFatalError)
          return ReadKeyRes::kFatalError;
        if (res == ReadStringRes::kNeedsMoreData)
          return ReadKeyRes::kNeedsMoreData;

        // We need to decrement from the pointer as the loop will increment
        // it back up.
        s--;
        next_token = NextToken::kColon;
        break;
      }
      case NextToken::kColon:
        if (*s != ':')
          return ReadKeyRes::kFatalError;
        next_token = NextToken::kValue;
        break;
      case NextToken::kValue:
        // Allowed value starting chars: [ { digit - "
        // Also allowed: true, false, null. For simplicities sake, we only check
        // against the first character as we're not trying to be super accurate.
        if (*s == '[' || *s == '{' || isdigit(*s) || *s == '-' || *s == '"' ||
            *s == 't' || *s == 'f' || *s == 'n') {
          *next = s;
          return ReadKeyRes::kFoundKey;
        }
        return ReadKeyRes::kFatalError;
    }
  }
  return ReadKeyRes::kNeedsMoreData;
}

base::Status ExtractValueForJsonKey(base::StringView dict,
                                    const std::string& key,
                                    std::optional<std::string>* value) {
  PERFETTO_DCHECK(dict.size() >= 2);

  const char* start = dict.data();
  const char* end = dict.data() + dict.size();

  enum ExtractValueState {
    kBeforeDict,
    kInsideDict,
    kAfterDict,
  };

  ExtractValueState state = kBeforeDict;
  for (const char* s = start; s < end;) {
    if (isspace(*s)) {
      ++s;
      continue;
    }

    if (state == kBeforeDict) {
      if (*s == '{') {
        ++s;
        state = kInsideDict;
        continue;
      }
      return base::ErrStatus("Unexpected character before JSON dict: '%c'", *s);
    }

    if (state == kAfterDict) {
      return base::ErrStatus("Unexpected character after JSON dict: '%c'", *s);
    }

    PERFETTO_DCHECK(state == kInsideDict);
    PERFETTO_DCHECK(s < end);

    if (*s == '}') {
      ++s;
      state = kAfterDict;
      continue;
    }

    std::string current_key;
    auto res = ReadOneJsonKey(s, end, &current_key, &s);
    if (res == ReadKeyRes::kEndOfDictionary)
      break;

    if (res == ReadKeyRes::kFatalError) {
      return base::ErrStatus(
          "Failure parsing JSON: encountered fatal error while parsing key for "
          "value: '%s'",
          FormatErrorContext(s, end).c_str());
    }

    if (res == ReadKeyRes::kNeedsMoreData) {
      return base::ErrStatus(
          "Failure parsing JSON: partial JSON dictionary: '%s'",
          FormatErrorContext(s, end).c_str());
    }

    PERFETTO_DCHECK(res == ReadKeyRes::kFoundKey);

    if (*s == '[') {
      return base::ErrStatus(
          "Failure parsing JSON: unsupported JSON dictionary with array: '%s'",
          FormatErrorContext(s, end).c_str());
    }

    std::string value_str;
    if (*s == '{') {
      base::StringView dict_str;
      ReadDictRes dict_res = ReadOneJsonDict(s, end, &dict_str, &s);
      if (dict_res == ReadDictRes::kNeedsMoreData ||
          dict_res == ReadDictRes::kEndOfArray ||
          dict_res == ReadDictRes::kEndOfTrace) {
        return base::ErrStatus(
            "Failure parsing JSON: unable to parse dictionary: '%s'",
            FormatErrorContext(s, end).c_str());
      }
      value_str = dict_str.ToStdString();
    } else if (*s == '"') {
      auto str_res = ReadOneJsonString(s, end, &value_str, &s);
      if (str_res == ReadStringRes::kNeedsMoreData ||
          str_res == ReadStringRes::kFatalError) {
        return base::ErrStatus(
            "Failure parsing JSON: unable to parse string: '%s",
            FormatErrorContext(s, end).c_str());
      }
    } else {
      const char* value_start = s;
      const char* value_end = end;
      for (; s < end; ++s) {
        if (*s == ',' || isspace(*s) || *s == '}') {
          value_end = s;
          break;
        }
      }
      value_str = std::string(value_start, value_end);
    }

    if (key == current_key) {
      *value = value_str;
      return base::OkStatus();
    }
  }

  if (state != kAfterDict) {
    return base::ErrStatus("Failure parsing JSON: malformed dictionary: '%s'",
                           FormatErrorContext(start, end).c_str());
  }

  *value = std::nullopt;
  return base::OkStatus();
}

ReadSystemLineRes ReadOneSystemTraceLine(const char* start,
                                         const char* end,
                                         std::string* line,
                                         const char** next) {
  bool is_escaping = false;
  for (const char* s = start; s < end; s++) {
    // If we get a quote character and we're not escaping, we are done with the
    // system trace string.
    if (*s == '"' && !is_escaping) {
      *next = s + 1;
      return ReadSystemLineRes::kEndOfSystemTrace;
    }

    // If we are escaping n, that means this is a new line which is a delimiter
    // for a system trace line.
    if (*s == 'n' && is_escaping) {
      *next = s + 1;
      return ReadSystemLineRes::kFoundLine;
    }

    base::Status status = AppendUnescapedCharacter(*s, is_escaping, line);
    if (!status.ok())
      return ReadSystemLineRes::kFatalError;

    // If we're in a string and we see a backslash and the last character was
    // not a backslash the next character is escaped:
    is_escaping = *s == '\\' && !is_escaping;
  }
  return ReadSystemLineRes::kNeedsMoreData;
}

JsonTraceTokenizer::JsonTraceTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}
JsonTraceTokenizer::~JsonTraceTokenizer() = default;

base::Status JsonTraceTokenizer::Parse(TraceBlobView blob) {
  PERFETTO_DCHECK(json::IsJsonSupported());

  buffer_.insert(buffer_.end(), blob.data(), blob.data() + blob.size());
  const char* buf = buffer_.data();
  const char* next = buf;
  const char* end = buf + buffer_.size();

  if (offset_ == 0) {
    // Strip leading whitespace.
    while (next != end && isspace(*next)) {
      next++;
    }
    if (next == end) {
      return base::ErrStatus(
          "Failure parsing JSON: first chunk has only whitespace");
    }

    // Trace could begin in any of these ways:
    // {"traceEvents":[{
    // { "traceEvents": [{
    // [{
    if (*next != '{' && *next != '[') {
      return base::ErrStatus(
          "Failure parsing JSON: first non-whitespace character is not [ or {");
    }

    // Figure out the format of the JSON file based on the first non-whitespace
    // character.
    format_ = *next == '{' ? TraceFormat::kOuterDictionary
                           : TraceFormat::kOnlyTraceEvents;

    // Skip the '[' or '{' character.
    next++;

    // Set our current position based on the format of the trace.
    position_ = format_ == TraceFormat::kOuterDictionary
                    ? TracePosition::kDictionaryKey
                    : TracePosition::kInsideTraceEventsArray;
  }
  RETURN_IF_ERROR(ParseInternal(next, end, &next));

  offset_ += static_cast<uint64_t>(next - buf);
  buffer_.erase(buffer_.begin(), buffer_.begin() + (next - buf));
  return base::OkStatus();
}

base::Status JsonTraceTokenizer::ParseInternal(const char* start,
                                               const char* end,
                                               const char** out) {
  PERFETTO_DCHECK(json::IsJsonSupported());

  switch (position_) {
    case TracePosition::kDictionaryKey:
      return HandleDictionaryKey(start, end, out);
    case TracePosition::kInsideSystemTraceEventsString:
      return HandleSystemTraceEvent(start, end, out);
    case TracePosition::kInsideTraceEventsArray:
      return HandleTraceEvent(start, end, out);
    case TracePosition::kEof: {
      return start == end
                 ? base::OkStatus()
                 : base::ErrStatus(
                       "Failure parsing JSON: tried to parse data after EOF");
    }
  }
  PERFETTO_FATAL("For GCC");
}

base::Status JsonTraceTokenizer::HandleTraceEvent(const char* start,
                                                  const char* end,
                                                  const char** out) {
  const char* next = start;
  while (next < end) {
    base::StringView unparsed;
    switch (ReadOneJsonDict(next, end, &unparsed, &next)) {
      case ReadDictRes::kEndOfArray: {
        if (format_ == TraceFormat::kOnlyTraceEvents) {
          position_ = TracePosition::kEof;
          return SetOutAndReturn(next, out);
        }

        position_ = TracePosition::kDictionaryKey;
        return ParseInternal(next, end, out);
      }
      case ReadDictRes::kEndOfTrace:
        position_ = TracePosition::kEof;
        return SetOutAndReturn(next, out);
      case ReadDictRes::kNeedsMoreData:
        return SetOutAndReturn(next, out);
      case ReadDictRes::kFoundDict:
        break;
    }

    std::optional<std::string> opt_raw_ts;
    RETURN_IF_ERROR(ExtractValueForJsonKey(unparsed, "ts", &opt_raw_ts));
    std::optional<int64_t> opt_ts =
        opt_raw_ts ? json::CoerceToTs(*opt_raw_ts) : std::nullopt;
    std::optional<std::string> opt_raw_dur;
    RETURN_IF_ERROR(ExtractValueForJsonKey(unparsed, "dur", &opt_raw_dur));
    std::optional<int64_t> opt_dur =
        opt_raw_dur ? json::CoerceToTs(*opt_raw_dur) : std::nullopt;
    int64_t ts = 0;
    if (opt_ts.has_value()) {
      ts = opt_ts.value();
    } else {
      // Metadata events may omit ts. In all other cases error:
      std::optional<std::string> opt_raw_ph;
      RETURN_IF_ERROR(ExtractValueForJsonKey(unparsed, "ph", &opt_raw_ph));
      if (!opt_raw_ph || *opt_raw_ph != "M") {
        context_->storage->IncrementStats(stats::json_tokenizer_failure);
        continue;
      }
    }
    context_->sorter->PushJsonValue(ts, unparsed.ToStdString(), opt_dur);
  }
  return SetOutAndReturn(next, out);
}

base::Status JsonTraceTokenizer::HandleDictionaryKey(const char* start,
                                                     const char* end,
                                                     const char** out) {
  if (format_ != TraceFormat::kOuterDictionary) {
    return base::ErrStatus(
        "Failure parsing JSON: illegal format when parsing dictionary key");
  }

  const char* next = start;
  std::string key;
  switch (ReadOneJsonKey(start, end, &key, &next)) {
    case ReadKeyRes::kFatalError:
      return base::ErrStatus(
          "Failure parsing JSON: encountered fatal error while parsing key");
    case ReadKeyRes::kEndOfDictionary:
      position_ = TracePosition::kEof;
      return SetOutAndReturn(next, out);
    case ReadKeyRes::kNeedsMoreData:
      // If we didn't manage to read the key we need to set |out| to |start|
      // (*not* |next|) to keep the state machine happy.
      return SetOutAndReturn(start, out);
    case ReadKeyRes::kFoundKey:
      break;
  }

  // ReadOneJsonKey should ensure that the first character of the value is
  // available.
  PERFETTO_CHECK(next < end);

  if (key == "traceEvents") {
    // Skip the [ character opening the array.
    if (*next != '[') {
      return base::ErrStatus(
          "Failure parsing JSON: traceEvents is not an array.");
    }
    next++;

    position_ = TracePosition::kInsideTraceEventsArray;
    return ParseInternal(next, end, out);
  }

  if (key == "systemTraceEvents") {
    // Skip the " character opening the string.
    if (*next != '"') {
      return base::ErrStatus(
          "Failure parsing JSON: systemTraceEvents is not an string.");
    }
    next++;

    position_ = TracePosition::kInsideSystemTraceEventsString;
    return ParseInternal(next, end, out);
  }

  if (key == "displayTimeUnit") {
    std::string time_unit;
    auto result = ReadOneJsonString(next, end, &time_unit, &next);
    if (result == ReadStringRes::kFatalError)
      return base::ErrStatus("Could not parse displayTimeUnit");
    context_->storage->IncrementStats(stats::json_display_time_unit);
    return ParseInternal(next, end, out);
  }

  // If we don't know the key for this JSON value just skip it.
  switch (SkipOneJsonValue(next, end, &next)) {
    case SkipValueRes::kFatalError:
      return base::ErrStatus(
          "Failure parsing JSON: error while parsing value for key %s",
          key.c_str());
    case SkipValueRes::kNeedsMoreData:
      // If we didn't manage to read the key *and* the value, we need to set
      // |out| to |start| (*not* |next|) to keep the state machine happy (as
      // we expect to always see a key before the value).
      return SetOutAndReturn(start, out);
    case SkipValueRes::kEndOfValue:
      return ParseInternal(next, end, out);
  }
  PERFETTO_FATAL("For GCC");
}

base::Status JsonTraceTokenizer::HandleSystemTraceEvent(const char* start,
                                                        const char* end,
                                                        const char** out) {
  if (format_ != TraceFormat::kOuterDictionary) {
    return base::ErrStatus(
        "Failure parsing JSON: illegal format when parsing system events");
  }

  const char* next = start;
  while (next < end) {
    std::string raw_line;
    switch (ReadOneSystemTraceLine(next, end, &raw_line, &next)) {
      case ReadSystemLineRes::kFatalError:
        return base::ErrStatus(
            "Failure parsing JSON: encountered fatal error while parsing "
            "event inside trace event string");
      case ReadSystemLineRes::kNeedsMoreData:
        return SetOutAndReturn(next, out);
      case ReadSystemLineRes::kEndOfSystemTrace:
        position_ = TracePosition::kDictionaryKey;
        return ParseInternal(next, end, out);
      case ReadSystemLineRes::kFoundLine:
        break;
    }

    if (base::StartsWith(raw_line, "#") || raw_line.empty())
      continue;

    SystraceLine line;
    RETURN_IF_ERROR(systrace_line_tokenizer_.Tokenize(raw_line, &line));
    context_->sorter->PushSystraceLine(std::move(line));
  }
  return SetOutAndReturn(next, out);
}

base::Status JsonTraceTokenizer::NotifyEndOfFile() {
  return position_ == TracePosition::kEof
             ? base::OkStatus()
             : base::ErrStatus("JSON trace file is incomplete");
}

}  // namespace perfetto::trace_processor
