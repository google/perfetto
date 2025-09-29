/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/util/json_writer.h"

#include <cmath>

#include "perfetto/ext/base/dynamic_string_writer.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_processor {

namespace {

// Helper function to write an escaped JSON string.
void WriteEscapedJsonString(base::DynamicStringWriter& writer,
                            std::string_view value) {
  writer.AppendChar('"');
  for (char c : value) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (c == '"') {
      writer.AppendLiteral("\\\"");
    } else if (c == '\\') {
      writer.AppendLiteral("\\\\");
    } else if (c == '\n') {
      writer.AppendLiteral("\\n");
    } else if (c == '\r') {
      writer.AppendLiteral("\\r");
    } else if (c == '\t') {
      writer.AppendLiteral("\\t");
    } else if (uc < 0x20) {
      writer.AppendLiteral("\\u00");
      writer.AppendChar("0123456789abcdef"[uc >> 4]);
      writer.AppendChar("0123456789abcdef"[uc & 0xf]);
    } else {
      writer.AppendChar(c);
    }
  }
  writer.AppendChar('"');
}

}  // namespace

JsonDictWriter::JsonDictWriter(base::DynamicStringWriter& writer)
    : writer_(writer), first_(true) {}

void JsonDictWriter::AddNull(std::string_view key) {
  Add(key, [](JsonValueWriter&& v) { std::move(v).WriteNull(); });
}

void JsonDictWriter::AddBool(std::string_view key, bool value) {
  Add(key, [value](JsonValueWriter&& v) { std::move(v).WriteBool(value); });
}

void JsonDictWriter::AddInt(std::string_view key, int64_t value) {
  Add(key, [value](JsonValueWriter&& v) { std::move(v).WriteInt(value); });
}

void JsonDictWriter::AddUint(std::string_view key, uint64_t value) {
  Add(key, [value](JsonValueWriter&& v) { std::move(v).WriteUint(value); });
}

void JsonDictWriter::AddDouble(std::string_view key, double value) {
  Add(key, [value](JsonValueWriter&& v) { std::move(v).WriteDouble(value); });
}

void JsonDictWriter::AddString(std::string_view key, std::string_view value) {
  Add(key, [value](JsonValueWriter&& v) { std::move(v).WriteString(value); });
}

void JsonDictWriter::AddKey(std::string_view key) {
  if (!first_) {
    writer_.AppendChar(',');
  }
  first_ = false;
  WriteEscapedJsonString(writer_, key);
  writer_.AppendChar(':');
}

JsonArrayWriter::JsonArrayWriter(base::DynamicStringWriter& writer)
    : writer_(writer), first_(true) {}

void JsonArrayWriter::AppendNull() {
  Append([](JsonValueWriter&& v) { std::move(v).WriteNull(); });
}

void JsonArrayWriter::AppendBool(bool value) {
  Append([value](JsonValueWriter&& v) { std::move(v).WriteBool(value); });
}

void JsonArrayWriter::AppendInt(int64_t value) {
  Append([value](JsonValueWriter&& v) { std::move(v).WriteInt(value); });
}

void JsonArrayWriter::AppendUint(uint64_t value) {
  Append([value](JsonValueWriter&& v) { std::move(v).WriteUint(value); });
}

void JsonArrayWriter::AppendDouble(double value) {
  Append([value](JsonValueWriter&& v) { std::move(v).WriteDouble(value); });
}

void JsonArrayWriter::AppendString(std::string_view value) {
  Append([value](JsonValueWriter&& v) { std::move(v).WriteString(value); });
}

void JsonArrayWriter::AddSeparator() {
  if (!first_) {
    writer_.AppendChar(',');
  }
  first_ = false;
}

JsonValueWriter::JsonValueWriter(base::DynamicStringWriter& writer)
    : writer_(writer) {}

void JsonValueWriter::WriteNull() && {
  writer_.AppendLiteral("null");
}

void JsonValueWriter::WriteBool(bool value) && {
  writer_.AppendString(value ? "true" : "false");
}

void JsonValueWriter::WriteInt(int64_t value) && {
  writer_.AppendInt(value);
}

void JsonValueWriter::WriteUint(uint64_t value) && {
  writer_.AppendUnsignedInt(value);
}

void JsonValueWriter::WriteDouble(double value) && {
  if (std::isnan(value)) {
    writer_.AppendLiteral("\"NaN\"");
  } else if (std::isinf(value) && value > 0) {
    writer_.AppendLiteral("\"Infinity\"");
  } else if (std::isinf(value) && value < 0) {
    writer_.AppendLiteral("\"-Infinity\"");
  } else {
    writer_.AppendDouble(value);
  }
}

void JsonValueWriter::WriteString(std::string_view value) && {
  WriteEscapedString(value);
}

void JsonValueWriter::WriteEscapedString(std::string_view value) {
  writer_.AppendChar('"');
  for (char c : value) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (c == '"') {
      writer_.AppendLiteral("\\\"");
    } else if (c == '\\') {
      writer_.AppendLiteral("\\\\");
    } else if (c == '\n') {
      writer_.AppendLiteral("\\n");
    } else if (c == '\r') {
      writer_.AppendLiteral("\\r");
    } else if (c == '\t') {
      writer_.AppendLiteral("\\t");
    } else if (uc < 0x20) {
      writer_.AppendLiteral("\\u00");
      writer_.AppendChar("0123456789abcdef"[uc >> 4]);
      writer_.AppendChar("0123456789abcdef"[uc & 0xf]);
    } else {
      writer_.AppendChar(c);
    }
  }
  writer_.AppendChar('"');
}

}  // namespace perfetto::trace_processor