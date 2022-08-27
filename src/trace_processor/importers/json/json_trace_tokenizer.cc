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

#include <memory>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/string_utils.h"

#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

namespace {

util::Status AppendUnescapedCharacter(char c,
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
        return util::ErrStatus("Illegal character in JSON");
    }
  } else if (c != '\\') {
    key->push_back(c);
  }
  return util::OkStatus();
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
  bool is_escaping = false;
  for (const char* s = start; s < end; s++) {
    // Control characters are not allowed in JSON strings.
    if (iscntrl(*s))
      return ReadStringRes::kFatalError;

    // If we get a quote character end of the string.
    if (*s == '"' && !is_escaping) {
      *next = s + 1;
      return ReadStringRes::kEndOfString;
    }

    util::Status status = AppendUnescapedCharacter(*s, is_escaping, key);
    if (!status.ok())
      return ReadStringRes::kFatalError;

    // If we're in a string and we see a backslash and the last character was
    // not a backslash the next character is escaped:
    is_escaping = *s == '\\' && !is_escaping;
  }
  return ReadStringRes::kNeedsMoreData;
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
      size_t len = static_cast<size_t>((s + 1) - dict_begin);
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

        // If we see anything else but a quote character here, this cannot be a
        // valid key.
        if (*s != '"')
          return ReadKeyRes::kFatalError;

        auto res = ReadOneJsonString(s + 1, end, key, &s);
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

util::Status ExtractValueForJsonKey(base::StringView dict,
                                    const std::string& key,
                                    base::Optional<std::string>* value) {
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
      return util::ErrStatus("Unexpected character before JSON dict");
    }

    if (state == kAfterDict)
      return util::ErrStatus("Unexpected character after JSON dict");

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

    if (res == ReadKeyRes::kFatalError)
      return util::ErrStatus("Failure parsing JSON: encountered fatal error");

    if (res == ReadKeyRes::kNeedsMoreData) {
      return util::ErrStatus("Failure parsing JSON: partial JSON dictionary");
    }

    PERFETTO_DCHECK(res == ReadKeyRes::kFoundKey);

    if (*s == '[') {
      return util::ErrStatus(
          "Failure parsing JSON: unsupported JSON dictionary with array");
    }

    std::string value_str;
    if (*s == '{') {
      base::StringView dict_str;
      ReadDictRes dict_res = ReadOneJsonDict(s, end, &dict_str, &s);
      if (dict_res == ReadDictRes::kNeedsMoreData ||
          dict_res == ReadDictRes::kEndOfArray ||
          dict_res == ReadDictRes::kEndOfTrace) {
        return util::ErrStatus(
            "Failure parsing JSON: unable to parse dictionary");
      }
      value_str = dict_str.ToStdString();
    } else if (*s == '"') {
      auto str_res = ReadOneJsonString(s + 1, end, &value_str, &s);
      if (str_res == ReadStringRes::kNeedsMoreData ||
          str_res == ReadStringRes::kFatalError) {
        return util::ErrStatus("Failure parsing JSON: unable to parse string");
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
      return util::OkStatus();
    }
  }

  if (state != kAfterDict)
    return util::ErrStatus("Failure parsing JSON: malformed dictionary");

  *value = base::nullopt;
  return util::OkStatus();
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

    util::Status status = AppendUnescapedCharacter(*s, is_escaping, line);
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

util::Status JsonTraceTokenizer::Parse(TraceBlobView blob) {
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
      return util::ErrStatus(
          "Failure parsing JSON: first chunk has only whitespace");
    }

    // Trace could begin in any of these ways:
    // {"traceEvents":[{
    // { "traceEvents": [{
    // [{
    if (*next != '{' && *next != '[') {
      return util::ErrStatus(
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
                    : TracePosition::kTraceEventsArray;
  }

  auto status = ParseInternal(next, end, &next);
  if (!status.ok())
    return status;

  offset_ += static_cast<uint64_t>(next - buf);
  buffer_.erase(buffer_.begin(), buffer_.begin() + (next - buf));
  return util::OkStatus();
}

util::Status JsonTraceTokenizer::ParseInternal(const char* start,
                                               const char* end,
                                               const char** out) {
  PERFETTO_DCHECK(json::IsJsonSupported());
  auto* trace_sorter = context_->sorter.get();

  const char* next = start;
  switch (position_) {
    case TracePosition::kDictionaryKey: {
      if (format_ != TraceFormat::kOuterDictionary) {
        return util::ErrStatus(
            "Failure parsing JSON: illegal format when parsing dictionary key");
      }

      std::string key;
      ReadKeyRes res = ReadOneJsonKey(start, end, &key, &next);
      if (res == ReadKeyRes::kFatalError)
        return util::ErrStatus("Failure parsing JSON: encountered fatal error");

      if (res == ReadKeyRes::kEndOfDictionary ||
          res == ReadKeyRes::kNeedsMoreData) {
        break;
      }

      if (key == "traceEvents") {
        position_ = TracePosition::kTraceEventsArray;
        return ParseInternal(next + 1, end, out);
      } else if (key == "systemTraceEvents") {
        position_ = TracePosition::kSystemTraceEventsString;
        return ParseInternal(next + 1, end, out);
      } else if (key == "androidProcessDump") {
        position_ = TracePosition::kAndroidProcessDumpString;
        return ParseInternal(next + 1, end, out);
      } else if (key == "metadata") {
        position_ = TracePosition::kWaitingForMetadataDictionary;
        return ParseInternal(next, end, out);
      } else if (key == "displayTimeUnit") {
        std::string time_unit;
        auto result = ReadOneJsonString(next + 1, end, &time_unit, &next);
        if (result == ReadStringRes::kFatalError)
          return util::ErrStatus("Could not parse displayTimeUnit");
        context_->storage->IncrementStats(stats::json_display_time_unit);
        return ParseInternal(next, end, out);
      } else if (key == "otherData") {
        base::StringView unparsed;
        const auto other = ReadOneJsonDict(next, end, &unparsed, &next);
        if (other == ReadDictRes::kEndOfArray)
          return util::ErrStatus(
              "Failure parsing JSON: Missing ] in otherData");
        if (other == ReadDictRes::kEndOfTrace)
          return util::ErrStatus(
              "Failure parsing JSON: Failed parsing otherData");
        if (other == ReadDictRes::kNeedsMoreData)
          return util::ErrStatus("Failure parsing JSON: otherData too large");
        return ParseInternal(next, end, out);
      } else {
        // If we don't recognize the key, just ignore the rest of the trace and
        // go to EOF.
        // TODO(lalitm): do something better here.
        position_ = TracePosition::kEof;
        break;
      }
    }
    case TracePosition::kSystemTraceEventsString: {
      if (format_ != TraceFormat::kOuterDictionary) {
        return util::ErrStatus(
            "Failure parsing JSON: illegal format when parsing system events");
      }

      while (next < end) {
        std::string raw_line;
        auto res = ReadOneSystemTraceLine(next, end, &raw_line, &next);
        if (res == ReadSystemLineRes::kFatalError)
          return util::ErrStatus(
              "Failure parsing JSON: encountered fatal error");

        if (res == ReadSystemLineRes::kNeedsMoreData)
          break;

        if (res == ReadSystemLineRes::kEndOfSystemTrace) {
          position_ = TracePosition::kDictionaryKey;
          return ParseInternal(next, end, out);
        }

        if (base::StartsWith(raw_line, "#") || raw_line.empty())
          continue;

        SystraceLine line;
        util::Status status =
            systrace_line_tokenizer_.Tokenize(raw_line, &line);
        if (!status.ok())
          return status;
        trace_sorter->PushSystraceLine(std::move(line));
      }
      break;
    }
    case TracePosition::kWaitingForMetadataDictionary: {
      if (format_ != TraceFormat::kOuterDictionary) {
        return util::ErrStatus(
            "Failure parsing JSON: illegal format when parsing metadata");
      }

      base::StringView unparsed;
      ReadDictRes res = ReadOneJsonDict(next, end, &unparsed, &next);
      if (res == ReadDictRes::kEndOfArray)
        return util::ErrStatus("Failure parsing JSON: encountered fatal error");
      if (res == ReadDictRes::kEndOfTrace ||
          res == ReadDictRes::kNeedsMoreData) {
        break;
      }

      // TODO(lalitm): read and ingest the relevant data inside |value|.
      position_ = TracePosition::kDictionaryKey;
      return ParseInternal(next, end, out);
    }
    case TracePosition::kAndroidProcessDumpString: {
      if (format_ != TraceFormat::kOuterDictionary) {
        return util::ErrStatus(
            "Failure parsing JSON: illegal format when parsing metadata");
      }

      std::string unparsed;
      ReadStringRes res = ReadOneJsonString(next, end, &unparsed, &next);
      if (res == ReadStringRes::kNeedsMoreData) {
        break;
      }
      if (res == ReadStringRes::kFatalError) {
        return base::ErrStatus(
            "Failure parsing JSON: illegal string when parsing "
            "androidProcessDump");
      }
      // TODO(lalitm): read and ingest the relevant data inside |unparsed|.
      position_ = TracePosition::kDictionaryKey;
      return ParseInternal(next, end, out);
    }
    case TracePosition::kTraceEventsArray: {
      while (next < end) {
        base::StringView unparsed;
        const auto res = ReadOneJsonDict(next, end, &unparsed, &next);
        if (res == ReadDictRes::kEndOfTrace ||
            res == ReadDictRes::kNeedsMoreData) {
          break;
        }

        if (res == ReadDictRes::kEndOfArray) {
          position_ = format_ == TraceFormat::kOuterDictionary
                          ? TracePosition::kDictionaryKey
                          : TracePosition::kEof;
          break;
        }

        base::Optional<std::string> opt_raw_ts;
        RETURN_IF_ERROR(ExtractValueForJsonKey(unparsed, "ts", &opt_raw_ts));
        base::Optional<int64_t> opt_ts =
            opt_raw_ts ? json::CoerceToTs(*opt_raw_ts) : base::nullopt;
        int64_t ts = 0;
        if (opt_ts.has_value()) {
          ts = opt_ts.value();
        } else {
          // Metadata events may omit ts. In all other cases error:
          base::Optional<std::string> opt_raw_ph;
          RETURN_IF_ERROR(ExtractValueForJsonKey(unparsed, "ph", &opt_raw_ph));
          if (!opt_raw_ph || *opt_raw_ph != "M") {
            context_->storage->IncrementStats(stats::json_tokenizer_failure);
            continue;
          }
        }
        trace_sorter->PushJsonValue(ts, unparsed.ToStdString());
      }
      break;
    }
    case TracePosition::kEof: {
      break;
    }
  }
  *out = next;
  return util::OkStatus();
}

void JsonTraceTokenizer::NotifyEndOfFile() {}

}  // namespace trace_processor
}  // namespace perfetto
