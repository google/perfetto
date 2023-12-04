
/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "perfetto/ext/trace_processor/rpc/query_result_serializer.h"

#include <deque>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto {
namespace trace_processor {

// For ASSERT_THAT(ElementsAre(...))
inline bool operator==(const SqlValue& a, const SqlValue& b) {
  if (a.type != b.type)
    return false;
  if (a.type == SqlValue::kString)
    return strcmp(a.string_value, b.string_value) == 0;
  if (a.type == SqlValue::kBytes) {
    if (a.bytes_count != b.bytes_count)
      return false;
    return memcmp(a.bytes_value, b.bytes_value, a.bytes_count) == 0;
  }
  return a.long_value == b.long_value;
}

inline std::ostream& operator<<(std::ostream& stream, const SqlValue& v) {
  stream << "SqlValue{";
  switch (v.type) {
    case SqlValue::kString:
      return stream << "\"" << v.string_value << "\"}";
    case SqlValue::kBytes:
      return stream << "Bytes[" << v.bytes_count << "]:"
                    << base::ToHex(reinterpret_cast<const char*>(v.bytes_value),
                                   v.bytes_count)
                    << "}";
    case SqlValue::kLong:
      return stream << "Long " << v.long_value << "}";
    case SqlValue::kDouble:
      return stream << "Double " << v.double_value << "}";
    case SqlValue::kNull:
      return stream << "NULL}";
  }
  return stream;
}

namespace {

using ::testing::ElementsAre;
using BatchProto = protos::pbzero::QueryResult::CellsBatch;
using ResultProto = protos::pbzero::QueryResult;

void RunQueryChecked(TraceProcessor* tp, const std::string& query) {
  auto iter = tp->ExecuteQuery(query);
  iter.Next();
  ASSERT_TRUE(iter.Status().ok()) << iter.Status().message();
}

// Implements a minimal deserializer for QueryResultSerializer.
class TestDeserializer {
 public:
  void SerializeAndDeserialize(QueryResultSerializer*);
  void DeserializeBuffer(const uint8_t* start, size_t size);

  std::vector<std::string> columns;
  std::vector<SqlValue> cells;
  std::string error;
  bool eof_reached = false;

 private:
  std::vector<std::unique_ptr<char[]>> copied_buf_;
};

void TestDeserializer::SerializeAndDeserialize(
    QueryResultSerializer* serializer) {
  std::vector<uint8_t> buf;
  error.clear();
  for (eof_reached = false; !eof_reached;) {
    serializer->Serialize(&buf);
    DeserializeBuffer(buf.data(), buf.size());
    buf.clear();
  }
}

void TestDeserializer::DeserializeBuffer(const uint8_t* start, size_t size) {
  ResultProto::Decoder result(start, size);
  error += result.error().ToStdString();
  for (auto it = result.column_names(); it; ++it)
    columns.push_back(it->as_std_string());

  for (auto batch_it = result.batch(); batch_it; ++batch_it) {
    ASSERT_FALSE(eof_reached);
    auto batch_bytes = batch_it->as_bytes();

    ResultProto::CellsBatch::Decoder batch(batch_bytes.data, batch_bytes.size);
    eof_reached = batch.is_last_batch();
    std::deque<int64_t> varints;
    std::deque<double> doubles;
    std::deque<std::string> blobs;

    bool parse_error = false;
    for (auto it = batch.varint_cells(&parse_error); it; ++it)
      varints.emplace_back(*it);

    for (auto it = batch.float64_cells(&parse_error); it; ++it)
      doubles.emplace_back(*it);

    for (auto it = batch.blob_cells(); it; ++it)
      blobs.emplace_back((*it).ToStdString());

    std::string merged_strings = batch.string_cells().ToStdString();
    std::deque<std::string> strings;
    for (size_t pos = 0; pos < merged_strings.size();) {
      // Will return npos for the last string, but it's fine
      size_t next_sep = merged_strings.find('\0', pos);
      strings.emplace_back(merged_strings.substr(pos, next_sep - pos));
      pos = next_sep == std::string::npos ? next_sep : next_sep + 1;
    }

    uint32_t num_cells = 0;
    for (auto it = batch.cells(&parse_error); it; ++it, ++num_cells) {
      uint8_t cell_type = static_cast<uint8_t>(*it);
      switch (cell_type) {
        case BatchProto::CELL_INVALID:
          break;
        case BatchProto::CELL_NULL:
          cells.emplace_back(SqlValue());
          break;
        case BatchProto::CELL_VARINT:
          ASSERT_GT(varints.size(), 0u);
          cells.emplace_back(SqlValue::Long(varints.front()));
          varints.pop_front();
          break;
        case BatchProto::CELL_FLOAT64:
          ASSERT_GT(doubles.size(), 0u);
          cells.emplace_back(SqlValue::Double(doubles.front()));
          doubles.pop_front();
          break;
        case BatchProto::CELL_STRING: {
          ASSERT_GT(strings.size(), 0u);
          const std::string& str = strings.front();
          copied_buf_.emplace_back(new char[str.size() + 1]);
          char* new_buf = copied_buf_.back().get();
          memcpy(new_buf, str.c_str(), str.size() + 1);
          cells.emplace_back(SqlValue::String(new_buf));
          strings.pop_front();
          break;
        }
        case BatchProto::CELL_BLOB: {
          ASSERT_GT(blobs.size(), 0u);
          auto bytes = blobs.front();
          copied_buf_.emplace_back(new char[bytes.size()]);
          memcpy(copied_buf_.back().get(), bytes.data(), bytes.size());
          cells.emplace_back(
              SqlValue::Bytes(copied_buf_.back().get(), bytes.size()));
          blobs.pop_front();
          break;
        }
        default:
          FAIL() << "Unknown cell type " << cell_type;
      }

      EXPECT_FALSE(parse_error);
    }
    if (columns.empty()) {
      EXPECT_EQ(num_cells, 0u);
    } else {
      EXPECT_EQ(num_cells % columns.size(), 0u);
    }
  }
}

TEST(QueryResultSerializerTest, ShortBatch) {
  auto tp = TraceProcessor::CreateInstance(trace_processor::Config());

  auto iter = tp->ExecuteQuery(
      "select 1 as i8, 128 as i16, 100000 as i32, 42001001001 as i64, 1e9 as "
      "f64, 'a_string' as str, cast('a_blob' as blob) as blb");
  QueryResultSerializer ser(std::move(iter));
  TestDeserializer deser;
  deser.SerializeAndDeserialize(&ser);

  EXPECT_THAT(deser.columns,
              ElementsAre("i8", "i16", "i32", "i64", "f64", "str", "blb"));
  EXPECT_THAT(deser.cells,
              ElementsAre(SqlValue::Long(1), SqlValue::Long(128),
                          SqlValue::Long(100000), SqlValue::Long(42001001001),
                          SqlValue::Double(1e9), SqlValue::String("a_string"),
                          SqlValue::Bytes("a_blob", 6)));
}

TEST(QueryResultSerializerTest, LongBatch) {
  auto tp = TraceProcessor::CreateInstance(trace_processor::Config());

  RunQueryChecked(tp.get(), "create virtual table win using window;");
  RunQueryChecked(tp.get(),
                  "update win set window_start=0, window_dur=8192, quantum=1 "
                  "where rowid = 0");

  auto iter = tp->ExecuteQuery(
      "select 'x' as x, ts, dur * 1.0 as dur, quantum_ts from win");
  QueryResultSerializer ser(std::move(iter));

  TestDeserializer deser;
  deser.SerializeAndDeserialize(&ser);

  ASSERT_THAT(deser.columns, ElementsAre("x", "ts", "dur", "quantum_ts"));
  ASSERT_EQ(deser.cells.size(), 4 * 8192u);
  for (uint32_t row = 0; row < 1024; row++) {
    uint32_t cell = row * 4;
    ASSERT_EQ(deser.cells[cell].type, SqlValue::kString);
    ASSERT_STREQ(deser.cells[cell].string_value, "x");

    ASSERT_EQ(deser.cells[cell + 1].type, SqlValue::kLong);
    ASSERT_EQ(deser.cells[cell + 1].long_value, row);

    ASSERT_EQ(deser.cells[cell + 2].type, SqlValue::kDouble);
    ASSERT_EQ(deser.cells[cell + 2].double_value, 1.0);

    ASSERT_EQ(deser.cells[cell + 3].type, SqlValue::kLong);
    ASSERT_EQ(deser.cells[cell + 3].long_value, row);
  }
}

TEST(QueryResultSerializerTest, BatchSaturatingBinaryPayload) {
  auto tp = TraceProcessor::CreateInstance(trace_processor::Config());

  RunQueryChecked(tp.get(), "create virtual table win using window;");
  RunQueryChecked(tp.get(),
                  "update win set window_start=0, window_dur=1024, quantum=1 "
                  "where rowid = 0");
  auto iter = tp->ExecuteQuery(
      "select 'x' as x, ts, dur * 1.0 as dur, quantum_ts from win");
  QueryResultSerializer ser(std::move(iter));
  ser.set_batch_size_for_testing(1024, 32);

  TestDeserializer deser;
  deser.SerializeAndDeserialize(&ser);

  ASSERT_THAT(deser.columns, ElementsAre("x", "ts", "dur", "quantum_ts"));
  ASSERT_EQ(deser.cells.size(), 1024 * 4u);
}

TEST(QueryResultSerializerTest, BatchSaturatingNumCells) {
  auto tp = TraceProcessor::CreateInstance(trace_processor::Config());

  RunQueryChecked(tp.get(), "create virtual table win using window;");
  RunQueryChecked(tp.get(),
                  "update win set window_start=0, window_dur=4, quantum=1 "
                  "where rowid = 0");
  auto iter = tp->ExecuteQuery(
      "select 'x' as x, ts, dur * 1.0 as dur, quantum_ts from win");
  QueryResultSerializer ser(std::move(iter));
  ser.set_batch_size_for_testing(16, 4096);

  TestDeserializer deser;
  deser.SerializeAndDeserialize(&ser);

  ASSERT_THAT(deser.columns, ElementsAre("x", "ts", "dur", "quantum_ts"));
  ASSERT_EQ(deser.cells.size(), 16u);
}

TEST(QueryResultSerializerTest, LargeStringAndBlobs) {
  auto tp = TraceProcessor::CreateInstance(trace_processor::Config());
  RunQueryChecked(tp.get(), "create table tab (colz);");

  std::minstd_rand0 rnd_engine(0);
  std::vector<SqlValue> expected;
  std::string sql_values;
  std::deque<std::string> string_buf;  // Needs stable pointers
  for (size_t n = 0; n < 32; n++) {
    std::string very_long_str;
    size_t len = (rnd_engine() % 4) * 32 * 1024;
    very_long_str.resize(len);
    for (size_t i = 0; i < very_long_str.size(); i++)
      very_long_str[i] = 'A' + ((n * 11 + i) % 25);

    if (n % 4 == 0) {
      sql_values += "(NULL),";
      expected.emplace_back(SqlValue());  // NULL.
    } else if (n % 4 == 1) {
      // Blob
      sql_values += "(X'" + base::ToHex(very_long_str) + "'),";
      string_buf.emplace_back(std::move(very_long_str));
      expected.emplace_back(
          SqlValue::Bytes(string_buf.back().data(), string_buf.back().size()));
    } else {
      sql_values += "('" + very_long_str + "'),";
      string_buf.emplace_back(std::move(very_long_str));
      expected.emplace_back(SqlValue::String(string_buf.back().c_str()));
    }
  }
  sql_values.resize(sql_values.size() - 1);  // Remove trailing comma.
  RunQueryChecked(tp.get(), "insert into tab (colz) values " + sql_values);

  auto iter = tp->ExecuteQuery("select colz from tab");
  QueryResultSerializer ser(std::move(iter));
  TestDeserializer deser;
  deser.SerializeAndDeserialize(&ser);
  ASSERT_EQ(deser.cells.size(), expected.size());
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(deser.cells[i], expected[i]) << "Cell " << i;
  }
}

TEST(QueryResultSerializerTest, RandomSizes) {
  auto tp = TraceProcessor::CreateInstance(trace_processor::Config());
  static constexpr uint32_t kNumCells = 3 * 1000;

  RunQueryChecked(tp.get(), "create table tab (a, b, c);");
  std::vector<SqlValue> expected;
  expected.reserve(kNumCells);
  std::deque<std::string> string_buf;  // Needs stable pointers
  std::minstd_rand0 rnd_engine(0);
  std::string insert_values;

  for (uint32_t i = 0; i < kNumCells; i++) {
    const uint32_t col = i % 3;
    if (col == 0)
      insert_values += "(";
    int type = rnd_engine() % 5;
    if (type == 0) {
      expected.emplace_back(SqlValue());  // NULL
      insert_values += "NULL";
    } else if (type == 1) {
      expected.emplace_back(SqlValue::Long(static_cast<long>(rnd_engine())));
      insert_values += std::to_string(expected.back().long_value);
    } else if (type == 2) {
      expected.emplace_back(
          SqlValue::Double(static_cast<double>(rnd_engine())));
      insert_values += std::to_string(expected.back().double_value);
    } else if (type == 3 || type == 4) {
      size_t len = (rnd_engine() % 5) * 32;
      std::string rndstr;
      rndstr.resize(len);
      for (size_t n = 0; n < len; n++)
        rndstr[n] = static_cast<char>(rnd_engine() % 256);
      auto rndstr_hex = base::ToHex(rndstr);
      if (type == 3) {
        insert_values += "\"" + rndstr_hex + "\"";
        string_buf.emplace_back(std::move(rndstr_hex));
        expected.emplace_back(SqlValue::String(string_buf.back().c_str()));

      } else {
        insert_values += "X'" + rndstr_hex + "'";
        string_buf.emplace_back(std::move(rndstr));
        expected.emplace_back(SqlValue::Bytes(string_buf.back().data(),
                                              string_buf.back().size()));
      }
    }

    if (col < 2) {
      insert_values += ",";
    } else {
      insert_values += "),";
      if (insert_values.size() > 1024 * 1024 || i == kNumCells - 1) {
        insert_values[insert_values.size() - 1] = ';';
        auto query = "insert into tab (a,b,c) values " + insert_values;
        insert_values = "";
        RunQueryChecked(tp.get(), query);
      }
    }
  }

  // Serialize and de-serialize with different batch and payload sizes.
  for (int rep = 0; rep < 10; rep++) {
    auto iter = tp->ExecuteQuery("select * from tab");
    QueryResultSerializer ser(std::move(iter));
    uint32_t cells_per_batch = 1 << (rnd_engine() % 8 + 2);
    uint32_t binary_payload_size = 1 << (rnd_engine() % 8 + 8);
    ser.set_batch_size_for_testing(cells_per_batch, binary_payload_size);
    TestDeserializer deser;
    deser.SerializeAndDeserialize(&ser);
    ASSERT_EQ(deser.cells.size(), expected.size());
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(deser.cells[i], expected[i]) << "Cell " << i;
    }
  }
}

TEST(QueryResultSerializerTest, ErrorBeforeStartingQuery) {
  auto tp = TraceProcessor::CreateInstance(trace_processor::Config());
  auto iter = tp->ExecuteQuery("insert into incomplete_input");
  QueryResultSerializer ser(std::move(iter));
  TestDeserializer deser;
  deser.SerializeAndDeserialize(&ser);
  EXPECT_EQ(deser.cells.size(), 0u);
  EXPECT_EQ(deser.error,
            "Traceback (most recent call last):\n  File \"stdin\" line 1 col "
            "1\n    insert into incomplete_input\n    ^\nincomplete input");
  EXPECT_TRUE(deser.eof_reached);
}

TEST(QueryResultSerializerTest, ErrorAfterSomeResults) {
  auto tp = TraceProcessor::CreateInstance(trace_processor::Config());
  RunQueryChecked(tp.get(), "create table tab (x)");
  RunQueryChecked(tp.get(), "insert into tab (x) values (0), (1), ('error')");
  auto iter = tp->ExecuteQuery("select str_split('a;b', ';', x) as s from tab");
  QueryResultSerializer ser(std::move(iter));
  TestDeserializer deser;
  deser.SerializeAndDeserialize(&ser);
  EXPECT_NE(deser.error, "");
  EXPECT_THAT(deser.cells,
              ElementsAre(SqlValue::String("a"), SqlValue::String("b")));
  EXPECT_TRUE(deser.eof_reached);
}

TEST(QueryResultSerializerTest, NoResultQuery) {
  auto tp = TraceProcessor::CreateInstance(trace_processor::Config());
  {
    auto iter = tp->ExecuteQuery("create table tab (x)");
    QueryResultSerializer ser(std::move(iter));
    TestDeserializer deser;
    deser.SerializeAndDeserialize(&ser);
    EXPECT_EQ(deser.error, "");
    EXPECT_EQ(deser.cells.size(), 0u);
    EXPECT_TRUE(deser.eof_reached);
  }

  // Check that the table has been created for real.
  {
    auto iter = tp->ExecuteQuery("select count(*) from tab");
    QueryResultSerializer ser(std::move(iter));
    TestDeserializer deser;
    deser.SerializeAndDeserialize(&ser);
    EXPECT_EQ(deser.error, "");
    EXPECT_EQ(deser.cells.size(), 1u);
    EXPECT_TRUE(deser.eof_reached);
  }
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
