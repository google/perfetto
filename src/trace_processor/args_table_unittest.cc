/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/args_table.h"
#include "src/trace_processor/scoped_db.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace perfetto {
namespace trace_processor {
namespace {

class ArgsTableUnittest : public ::testing::Test {
 public:
  ArgsTableUnittest() {
    sqlite3* db = nullptr;
    PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    db_.reset(db);

    context_.storage.reset(new TraceStorage());
    ArgsTable::RegisterTable(db_.get(), context_.storage.get());
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

  ~ArgsTableUnittest() override { context_.storage->ResetStorage(); }

 protected:
  TraceProcessorContext context_;
  ScopedDb db_;
  ScopedStmt stmt_;
};

TEST_F(ArgsTableUnittest, IntValue) {
  static const char kFlatKey[] = "flat_key";
  static const char kKey[] = "key";
  static const int kValue = 123;

  TraceStorage::Args::Arg arg;
  arg.flat_key = context_.storage->InternString(kFlatKey);
  arg.key = context_.storage->InternString(kKey);
  arg.value = Variadic::Integer(kValue);

  context_.storage->mutable_args()->AddArgSet({arg}, 0, 1);

  PrepareValidStatement("SELECT * FROM args");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), 1);             // arg_set_id
  ASSERT_STREQ(GetColumnAsText(1), kFlatKey);              // flat_key
  ASSERT_STREQ(GetColumnAsText(2), kKey);                  // key
  ASSERT_EQ(sqlite3_column_int(*stmt_, 3), kValue);        // int_value
  ASSERT_EQ(sqlite3_column_type(*stmt_, 4), SQLITE_NULL);  // string_value
  ASSERT_EQ(sqlite3_column_type(*stmt_, 5), SQLITE_NULL);  // real_value

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(ArgsTableUnittest, StringValue) {
  static const char kFlatKey[] = "flat_key";
  static const char kKey[] = "key";
  static const char kValue[] = "123";

  TraceStorage::Args::Arg arg;
  arg.flat_key = context_.storage->InternString(kFlatKey);
  arg.key = context_.storage->InternString(kKey);
  arg.value = Variadic::String(context_.storage->InternString(kValue));

  context_.storage->mutable_args()->AddArgSet({arg}, 0, 1);

  PrepareValidStatement("SELECT * FROM args");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), 1);             // arg_set_id
  ASSERT_STREQ(GetColumnAsText(1), kFlatKey);              // flat_key
  ASSERT_STREQ(GetColumnAsText(2), kKey);                  // key
  ASSERT_EQ(sqlite3_column_type(*stmt_, 3), SQLITE_NULL);  // int_value
  ASSERT_STREQ(GetColumnAsText(4), kValue);                // string_value
  ASSERT_EQ(sqlite3_column_type(*stmt_, 5), SQLITE_NULL);  // real_value

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(ArgsTableUnittest, RealValue) {
  static const char kFlatKey[] = "flat_key";
  static const char kKey[] = "key";
  static const double kValue = 0.123;

  TraceStorage::Args::Arg arg;
  arg.flat_key = context_.storage->InternString(kFlatKey);
  arg.key = context_.storage->InternString(kKey);
  arg.value = Variadic::Real(kValue);

  context_.storage->mutable_args()->AddArgSet({arg}, 0, 1);

  PrepareValidStatement("SELECT * FROM args");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), 1);             // arg_set_id
  ASSERT_STREQ(GetColumnAsText(1), kFlatKey);              // flat_key
  ASSERT_STREQ(GetColumnAsText(2), kKey);                  // key
  ASSERT_EQ(sqlite3_column_type(*stmt_, 3), SQLITE_NULL);  // int_value
  ASSERT_EQ(sqlite3_column_type(*stmt_, 4), SQLITE_NULL);  // string_value
  ASSERT_EQ(sqlite3_column_double(*stmt_, 5), kValue);     // real_value

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
