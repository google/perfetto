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

#ifndef SRC_TRACE_PROCESSOR_UTIL_JSON_WRITER_H_
#define SRC_TRACE_PROCESSOR_UTIL_JSON_WRITER_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "perfetto/ext/base/dynamic_string_writer.h"

namespace perfetto::trace_processor {

class JsonDictWriter;
class JsonArrayWriter;
class JsonValueWriter;

// Main entry point for writing JSON.
// Usage:
//   std::string json = write([](JsonValueWriter writer) {
//     std::move(writer).WriteDict([](JsonDictWriter dict) {
//       dict.AddString("hello", "world");
//     });
//   });
template <typename ValueWriterFn>
std::string write(ValueWriterFn&& value_writer);

// Writes a JSON dictionary.
// Usage example:
//   dict.AddString("key", "value");
//   dict.AddDict("nested", [](JsonDictWriter& nested) {
//     nested.AddInt("count", 42);
//   });
class JsonDictWriter {
 public:
  explicit JsonDictWriter(base::DynamicStringWriter& writer);
  JsonDictWriter(const JsonDictWriter&) = delete;
  JsonDictWriter& operator=(const JsonDictWriter&) = delete;

  // Primitive values.
  void AddNull(std::string_view key);
  void AddBool(std::string_view key, bool value);
  void AddInt(std::string_view key, int64_t value);
  void AddUint(std::string_view key, uint64_t value);
  void AddDouble(std::string_view key, double value);
  void AddString(std::string_view key, std::string_view value);

  // Writes a nested dictionary. `DictWriterFn` should be a function taking
  // `JsonDictWriter`.
  template <typename DictWriterFn>
  void AddDict(std::string_view key, DictWriterFn&& dict_writer);

  // Writes a nested dictionary. `ArrayWriterFn` should be a function taking
  // `JsonArrayWriter`.
  template <typename ArrayWriterFn>
  void AddArray(std::string_view key, ArrayWriterFn&& array_writer);

  // Writes a generic value. `ValueWriterFn` should be a function taking
  // `JsonValueWriter`.
  template <typename ValueWriterFn>
  void Add(std::string_view key, ValueWriterFn&& value_writer);

 private:
  void AddKey(std::string_view key);

  base::DynamicStringWriter& writer_;
  bool first_;
};

// Writes a JSON array.
// Usage example:
//   array.AppendString("item1");
//   array.AppendDict([](JsonDictWriter& dict) {
//     dict.AddString("key", "value");
//   });
class JsonArrayWriter {
 public:
  explicit JsonArrayWriter(base::DynamicStringWriter& writer);
  JsonArrayWriter(const JsonArrayWriter&) = delete;
  JsonArrayWriter& operator=(const JsonArrayWriter&) = delete;

  // Primitive values.
  void AppendNull();
  void AppendBool(bool value);
  void AppendInt(int64_t value);
  void AppendUint(uint64_t value);
  void AppendDouble(double value);
  void AppendString(std::string_view value);

  // Writes a nested dictionary. `DictWriterFn` should be a function taking
  // `JsonDictWriter`.
  template <typename DictWriterFn>
  void AppendDict(DictWriterFn&& dict_writer);

  // Writes a nested dictionary. `ArrayWriterFn` should be a function taking
  // `JsonArrayWriter`.
  template <typename ArrayWriterFn>
  void AppendArray(ArrayWriterFn&& array_writer);

  // Writes a generic value. `ValueWriterFn` should be a function taking
  // `JsonValueWriter`.
  template <typename ValueWriterFn>
  void Append(ValueWriterFn&& value_writer);

 private:
  void AddSeparator();

  base::DynamicStringWriter& writer_;
  bool first_;
};

// Generic value writer.
// Usage example:
//   [](JsonValueWriter writer) {
//     std::move(writer).WriteString("foo");
//   });
class JsonValueWriter {
 public:
  explicit JsonValueWriter(base::DynamicStringWriter& writer);
  JsonValueWriter(const JsonValueWriter&) = delete;
  JsonValueWriter& operator=(const JsonValueWriter&) = delete;

  // Primitive values.
  void WriteNull() &&;
  void WriteBool(bool value) &&;
  void WriteInt(int64_t value) &&;
  void WriteUint(uint64_t value) &&;
  void WriteDouble(double value) &&;
  void WriteString(std::string_view value) &&;

  // Writes a dictionary. `DictWriterFn` should be a function taking
  // `JsonDictWriter`.
  template <typename DictWriterFn>
  void WriteDict(DictWriterFn&& dict_writer) &&;

  // Writes an array. `ArrayWriterFn` should be a function taking
  // `JsonArrayWriter`.
  template <typename ArrayWriterFn>
  void WriteArray(ArrayWriterFn&& array_writer) &&;

 private:
  void WriteEscapedString(std::string_view value);

  base::DynamicStringWriter& writer_;
};

template <typename ValueWriterFn>
void JsonDictWriter::Add(std::string_view key, ValueWriterFn&& value_writer) {
  AddKey(key);
  JsonValueWriter writer(writer_);
  value_writer(std::move(writer));
}

template <typename DictWriterFn>
void JsonDictWriter::AddDict(std::string_view key, DictWriterFn&& dict_writer) {
  AddKey(key);
  writer_.AppendChar('{');
  JsonDictWriter dict(writer_);
  dict_writer(dict);
  writer_.AppendChar('}');
}

template <typename ArrayWriterFn>
void JsonDictWriter::AddArray(std::string_view key,
                              ArrayWriterFn&& array_writer) {
  AddKey(key);
  writer_.AppendChar('[');
  JsonArrayWriter array(writer_);
  array_writer(array);
  writer_.AppendChar(']');
}

template <typename ValueWriterFn>
void JsonArrayWriter::Append(ValueWriterFn&& value_writer) {
  AddSeparator();
  JsonValueWriter writer(writer_);
  value_writer(std::move(writer));
}

template <typename DictWriterFn>
void JsonArrayWriter::AppendDict(DictWriterFn&& dict_writer) {
  AddSeparator();
  writer_.AppendChar('{');
  JsonDictWriter dict(writer_);
  dict_writer(dict);
  writer_.AppendChar('}');
}

template <typename ArrayWriterFn>
void JsonArrayWriter::AppendArray(ArrayWriterFn&& array_writer) {
  AddSeparator();
  writer_.AppendChar('[');
  JsonArrayWriter array(writer_);
  array_writer(array);
  writer_.AppendChar(']');
}

template <typename DictWriterFn>
void JsonValueWriter::WriteDict(DictWriterFn&& dict_writer) && {
  writer_.AppendChar('{');
  JsonDictWriter dict(writer_);
  dict_writer(dict);
  writer_.AppendChar('}');
}

template <typename ArrayWriterFn>
void JsonValueWriter::WriteArray(ArrayWriterFn&& array_writer) && {
  writer_.AppendChar('[');
  JsonArrayWriter array(writer_);
  array_writer(array);
  writer_.AppendChar(']');
}

template <typename ValueWriterFn>
std::string write(ValueWriterFn&& value_writer) {
  base::DynamicStringWriter writer;
  JsonValueWriter json_value_writer(writer);
  value_writer(std::move(json_value_writer));
  return writer.GetStringView().ToStdString();
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_JSON_WRITER_H_