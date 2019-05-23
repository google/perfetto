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

#include "src/trace_processor/metadata_table.h"
#include "src/trace_processor/scoped_db.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

class MetadataTableUnittest : public ::testing::Test {
 public:
  MetadataTableUnittest() {
    sqlite3* db = nullptr;
    PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    db_.reset(db);

    context_.storage.reset(new TraceStorage());
    MetadataTable::RegisterTable(db_.get(), context_.storage.get());
  }

  void PrepareValidStatement(const std::string& sql) {
    int size = static_cast<int>(sql.size());
    sqlite3_stmt* stmt;
    ASSERT_EQ(sqlite3_prepare_v2(*db_, sql.c_str(), size, &stmt, nullptr),
              SQLITE_OK);
    stmt_.reset(stmt);
  }

  const char* GetColumnAsText(int colId) {
    return reinterpret_cast<const char*>(sqlite3_column_text(*stmt_, colId));
  }

  ~MetadataTableUnittest() override { context_.storage->ResetStorage(); }

 protected:
  TraceProcessorContext context_;
  ScopedDb db_;
  ScopedStmt stmt_;
};

TEST_F(MetadataTableUnittest, NoEntries) {
  PrepareValidStatement("SELECT * FROM metadata");
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(MetadataTableUnittest, SingleStringValue) {
  static const char kName[] = "benchmark";
  Variadic value = Variadic::String(context_.storage->InternString(kName));
  context_.storage->SetMetadata(metadata::benchmark_name, value);

  PrepareValidStatement("SELECT * FROM metadata");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_STREQ(GetColumnAsText(0), "benchmark_name");      // name
  ASSERT_STREQ(GetColumnAsText(1), "single");              // key_type
  ASSERT_EQ(sqlite3_column_type(*stmt_, 2), SQLITE_NULL);  // int_value
  ASSERT_STREQ(GetColumnAsText(3), kName);                 // str_value
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(MetadataTableUnittest, SingleIntegerValue) {
  static const int64_t kTimestamp = 1234567890;
  Variadic value = Variadic::Integer(kTimestamp);
  context_.storage->SetMetadata(metadata::benchmark_story_run_time_us, value);

  PrepareValidStatement("SELECT * FROM metadata");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_STREQ(GetColumnAsText(0), "benchmark_story_run_time_us");  // name
  ASSERT_STREQ(GetColumnAsText(1), "single");                       // key_type
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), kTimestamp);           // int_value
  ASSERT_EQ(sqlite3_column_type(*stmt_, 3), SQLITE_NULL);           // str_value
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(MetadataTableUnittest, MultipleStringValues) {
  static const char kTag1[] = "foo";
  static const char kTag2[] = "bar";
  Variadic tag1 = Variadic::String(context_.storage->InternString(kTag1));
  Variadic tag2 = Variadic::String(context_.storage->InternString(kTag2));
  context_.storage->AppendMetadata(metadata::benchmark_story_tags, tag1);
  context_.storage->AppendMetadata(metadata::benchmark_story_tags, tag2);

  PrepareValidStatement("SELECT * FROM metadata");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_STREQ(GetColumnAsText(0), "benchmark_story_tags");  // name
  ASSERT_STREQ(GetColumnAsText(1), "multi");                 // key_type
  ASSERT_EQ(sqlite3_column_type(*stmt_, 2), SQLITE_NULL);    // int_value
  ASSERT_STREQ(GetColumnAsText(3), kTag1);                   // str_value

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_STREQ(GetColumnAsText(0), "benchmark_story_tags");  // name
  ASSERT_STREQ(GetColumnAsText(1), "multi");                 // key_type
  ASSERT_EQ(sqlite3_column_type(*stmt_, 2), SQLITE_NULL);    // int_value
  ASSERT_STREQ(GetColumnAsText(3), kTag2);                   // str_value

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
