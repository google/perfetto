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
#include "test/gtest_and_gmock.h"

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

  void AssertArgRowValues(int arg_set_id,
                          const char* flat_key,
                          const char* key,
                          base::Optional<int64_t> int_value,
                          base::Optional<const char*> string_value,
                          base::Optional<double> real_value);

  ~ArgsTableUnittest() override { context_.storage->ResetStorage(); }

 protected:
  TraceProcessorContext context_;
  ScopedDb db_;
  ScopedStmt stmt_;
};

// Test helper.
void ArgsTableUnittest::AssertArgRowValues(
    int arg_set_id,
    const char* flat_key,
    const char* key,
    base::Optional<int64_t> int_value,
    base::Optional<const char*> string_value,
    base::Optional<double> real_value) {
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), arg_set_id);
  ASSERT_STREQ(GetColumnAsText(1), flat_key);
  ASSERT_STREQ(GetColumnAsText(2), key);
  if (int_value.has_value()) {
    ASSERT_EQ(sqlite3_column_int64(*stmt_, 3), int_value.value());
  } else {
    ASSERT_EQ(sqlite3_column_type(*stmt_, 3), SQLITE_NULL);
  }
  if (string_value.has_value()) {
    ASSERT_STREQ(GetColumnAsText(4), string_value.value());
  } else {
    ASSERT_EQ(sqlite3_column_type(*stmt_, 4), SQLITE_NULL);
  }
  if (real_value.has_value()) {
    ASSERT_DOUBLE_EQ(sqlite3_column_double(*stmt_, 5), real_value.value());
  } else {
    ASSERT_EQ(sqlite3_column_type(*stmt_, 5), SQLITE_NULL);
  }
}

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
  AssertArgRowValues(1, kFlatKey, kKey, kValue, base::nullopt, base::nullopt);
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
  AssertArgRowValues(1, kFlatKey, kKey, base::nullopt, kValue, base::nullopt);
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
  AssertArgRowValues(1, kFlatKey, kKey, base::nullopt, base::nullopt, kValue);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(ArgsTableUnittest, BoolValueTreatedAsInt) {
  static const char kFlatKey[] = "flat_key";
  static const char kKey[] = "key";
  static const bool kValue = true;

  TraceStorage::Args::Arg arg;
  arg.flat_key = context_.storage->InternString(kFlatKey);
  arg.key = context_.storage->InternString(kKey);
  arg.value = Variadic::Boolean(kValue);

  context_.storage->mutable_args()->AddArgSet({arg}, 0, 1);

  // Boolean returned in the "int_value" column, and is comparable to an integer
  // literal.
  PrepareValidStatement("SELECT * FROM args WHERE int_value = 1");
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, kValue, base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(ArgsTableUnittest, PointerValueTreatedAsInt) {
  static const uint64_t kSmallValue = 1ull << 30;
  static const uint64_t kTopBitSetValue = 1ull << 63;

  TraceStorage::Args::Arg arg;
  arg.flat_key = context_.storage->InternString("flat_key_small");
  arg.key = context_.storage->InternString("key_small");
  arg.value = Variadic::Pointer(kSmallValue);

  TraceStorage::Args::Arg arg2;
  arg2.flat_key = context_.storage->InternString("flat_key_large");
  arg2.key = context_.storage->InternString("key_large");
  arg2.value = Variadic::Pointer(kTopBitSetValue);

  context_.storage->mutable_args()->AddArgSet({arg, arg2}, 0, 2);

  // Pointer returned in the "int_value" column, as a signed 64 bit. And is
  // comparable to an integer literal.

  static const int64_t kExpectedSmallValue = static_cast<int64_t>(kSmallValue);
  PrepareValidStatement(std::string("SELECT * FROM args WHERE int_value = ") +
                        std::to_string(kExpectedSmallValue));
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, "flat_key_small", "key_small", kExpectedSmallValue,
                     base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);

  static const int64_t kExpectedTopBitSetValue =
      static_cast<int64_t>(kTopBitSetValue);  // negative
  PrepareValidStatement(std::string("SELECT * FROM args WHERE int_value = ") +
                        std::to_string(kExpectedTopBitSetValue));
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, "flat_key_large", "key_large", kExpectedTopBitSetValue,
                     base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(ArgsTableUnittest, UintValueTreatedAsInt) {
  static const uint64_t kSmallValue = 1ull << 30;
  static const uint64_t kTopBitSetValue = 1ull << 63;

  TraceStorage::Args::Arg arg;
  arg.flat_key = context_.storage->InternString("flat_key_small");
  arg.key = context_.storage->InternString("key_small");
  arg.value = Variadic::UnsignedInteger(kSmallValue);

  TraceStorage::Args::Arg arg2;
  arg2.flat_key = context_.storage->InternString("flat_key_large");
  arg2.key = context_.storage->InternString("key_large");
  arg2.value = Variadic::UnsignedInteger(kTopBitSetValue);

  context_.storage->mutable_args()->AddArgSet({arg, arg2}, 0, 2);

  // Unsigned returned in the "int_value" column, as a signed 64 bit. And is
  // comparable to an integer literal.

  static const int64_t kExpectedSmallValue = static_cast<int64_t>(kSmallValue);
  PrepareValidStatement(std::string("SELECT * FROM args WHERE int_value = ") +
                        std::to_string(kExpectedSmallValue));
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, "flat_key_small", "key_small", kExpectedSmallValue,
                     base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);

  static const int64_t kExpectedTopBitSetValue =
      static_cast<int64_t>(kTopBitSetValue);  // negative
  PrepareValidStatement(std::string("SELECT * FROM args WHERE int_value = ") +
                        std::to_string(kExpectedTopBitSetValue));
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, "flat_key_large", "key_large", kExpectedTopBitSetValue,
                     base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(ArgsTableUnittest, IntegerLikeValuesSortByIntRepresentation) {
  static const char kFlatKey[] = "flat_key";
  static const char kKey[] = "key";

  TraceStorage::Args::Arg bool_arg_true;
  bool_arg_true.flat_key = context_.storage->InternString(kFlatKey);
  bool_arg_true.key = context_.storage->InternString(kKey);
  bool_arg_true.value = Variadic::Boolean(true);

  TraceStorage::Args::Arg bool_arg_false;
  bool_arg_false.flat_key = context_.storage->InternString(kFlatKey);
  bool_arg_false.key = context_.storage->InternString(kKey);
  bool_arg_false.value = Variadic::Boolean(false);

  TraceStorage::Args::Arg pointer_arg_42;
  pointer_arg_42.flat_key = context_.storage->InternString(kFlatKey);
  pointer_arg_42.key = context_.storage->InternString(kKey);
  pointer_arg_42.value = Variadic::Pointer(42);

  TraceStorage::Args::Arg unsigned_arg_10;
  unsigned_arg_10.flat_key = context_.storage->InternString(kFlatKey);
  unsigned_arg_10.key = context_.storage->InternString(kKey);
  unsigned_arg_10.value = Variadic::UnsignedInteger(10);

  // treated as null by the int_value column
  TraceStorage::Args::Arg string_arg;
  string_arg.flat_key = context_.storage->InternString(kFlatKey);
  string_arg.key = context_.storage->InternString(kKey);
  string_arg.value =
      Variadic::String(context_.storage->InternString("string_content"));

  context_.storage->mutable_args()->AddArgSet(
      {bool_arg_true, bool_arg_false, pointer_arg_42, unsigned_arg_10,
       string_arg},
      0, 5);

  // Ascending sort by int representations:
  // { null (string), 0 (false), 1 (true), 10, 42 }
  PrepareValidStatement("SELECT * FROM args ORDER BY int_value ASC");
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, base::nullopt, "string_content",
                     base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, 0, base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, 1, base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, 10, base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, 42, base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);

  // Desceding order.
  PrepareValidStatement("SELECT * FROM args ORDER BY int_value DESC");
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, 42, base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, 10, base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, 1, base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, 0, base::nullopt, base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  AssertArgRowValues(1, kFlatKey, kKey, base::nullopt, "string_content",
                     base::nullopt);
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
