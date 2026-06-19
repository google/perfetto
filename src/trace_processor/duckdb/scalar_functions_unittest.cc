/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/duckdb/scalar_functions.h"

#include <cmath>
#include <string>
#include <unordered_set>

#include "duckdb.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

class ScalarFunctionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(duckdb_open(nullptr, &db_), DuckDBSuccess);
    ASSERT_EQ(duckdb_connect(db_, &con_), DuckDBSuccess);
    auto status = RegisterScalarFunctions(con_, &registered_);
    ASSERT_TRUE(status.ok()) << status.c_message();
  }
  void TearDown() override {
    duckdb_disconnect(&con_);
    duckdb_close(&db_);
  }

  // Runs a single-row, single-column query and returns the value as a string
  // ("NULL" for SQL NULL). Fails the test on a query error.
  std::string QueryString(const std::string& sql) {
    duckdb_result r;
    if (duckdb_query(con_, sql.c_str(), &r) == DuckDBError) {
      std::string err = duckdb_result_error(&r);
      duckdb_destroy_result(&r);
      ADD_FAILURE() << "query failed: " << sql << " :: " << err;
      return "<error>";
    }
    char* v = duckdb_value_varchar(&r, 0, 0);
    std::string out = v ? v : "NULL";
    duckdb_free(v);
    duckdb_destroy_result(&r);
    return out;
  }

  bool QueryErrors(const std::string& sql) {
    duckdb_result r;
    bool err = duckdb_query(con_, sql.c_str(), &r) == DuckDBError;
    duckdb_destroy_result(&r);
    return err;
  }

  duckdb_database db_ = nullptr;
  duckdb_connection con_ = nullptr;
  std::unordered_set<std::string> registered_;
};

TEST_F(ScalarFunctionsTest, RegisteredNamesIncludeSurfaceAndIntrinsic) {
  EXPECT_TRUE(registered_.count("ln"));
  EXPECT_TRUE(registered_.count("__intrinsic_ln"));
  EXPECT_TRUE(registered_.count("exp"));
  EXPECT_TRUE(registered_.count("sqrt"));
  EXPECT_TRUE(registered_.count("regexp_extract"));
  EXPECT_TRUE(registered_.count("__intrinsic_regexp_extract"));
  EXPECT_TRUE(registered_.count("unhex"));
  EXPECT_TRUE(registered_.count("__intrinsic_unhex"));
}

TEST_F(ScalarFunctionsTest, Ln) {
  // ln(e) ~= 1; ln(1) == 0; ln(<=0) and ln(NULL) -> NULL.
  EXPECT_EQ(QueryString("SELECT ln(1.0)"), "0.0");
  EXPECT_EQ(QueryString("SELECT CAST(round(ln(exp(2.0))) AS INTEGER)"), "2");
  EXPECT_EQ(QueryString("SELECT ln(0.0)"), "NULL");
  EXPECT_EQ(QueryString("SELECT ln(-1.0)"), "NULL");
  EXPECT_EQ(QueryString("SELECT ln(NULL)"), "NULL");
  // Text argument -> NULL (VARCHAR overload), not an error.
  EXPECT_EQ(QueryString("SELECT ln('abc')"), "NULL");
  // Intrinsic alias works too.
  EXPECT_EQ(QueryString("SELECT __intrinsic_ln(1.0)"), "0.0");
}

TEST_F(ScalarFunctionsTest, ExpSqrt) {
  EXPECT_EQ(QueryString("SELECT CAST(exp(1.0) * 1000 AS INTEGER)"), "2718");
  EXPECT_EQ(QueryString("SELECT exp(NULL)"), "NULL");
  EXPECT_EQ(QueryString("SELECT exp('abc')"), "NULL");
  EXPECT_EQ(QueryString("SELECT CAST(sqrt(4.0) AS INTEGER)"), "2");
  EXPECT_EQ(QueryString("SELECT sqrt(NULL)"), "NULL");
  EXPECT_EQ(QueryString("SELECT sqrt('abc')"), "NULL");
}

TEST_F(ScalarFunctionsTest, RegexpExtract) {
  // Group 1 when the (single) group matched.
  EXPECT_EQ(QueryString("SELECT regexp_extract('abcde', 'b(c)d')"), "c");
  EXPECT_EQ(QueryString("SELECT regexp_extract('abcde', 'a(b)cde')"), "b");
  // No match -> NULL.
  EXPECT_EQ(QueryString("SELECT regexp_extract('abcde', 'fgh')"), "NULL");
  // Optional group that did not match -> full match.
  EXPECT_EQ(QueryString("SELECT regexp_extract('ac', 'a(b)?c')"), "ac");
  EXPECT_EQ(QueryString("SELECT regexp_extract('abc', 'a(b)?c')"), "b");
  // NULL input -> NULL.
  EXPECT_EQ(QueryString("SELECT regexp_extract(NULL, 'x')"), "NULL");
  // More than one group -> error.
  EXPECT_TRUE(QueryErrors("SELECT regexp_extract('abc', '(a)(b)')"));
}

TEST_F(ScalarFunctionsTest, Unhex) {
  EXPECT_EQ(QueryString("SELECT unhex('0xF')"), "15");
  EXPECT_EQ(QueryString("SELECT unhex('F')"), "15");
  EXPECT_EQ(QueryString("SELECT unhex('  0Xf  ')"), "15");
  EXPECT_EQ(QueryString("SELECT unhex('0')"), "0");
  EXPECT_EQ(QueryString("SELECT unhex(NULL)"), "NULL");
  EXPECT_EQ(QueryString("SELECT unhex('0x58646cfa')"), "1482976506");
  EXPECT_TRUE(QueryErrors("SELECT unhex('zz')"));
}

// A multi-row query proves the vectorized trampoline handles a whole chunk
// (mixed valid + NULL) correctly, not just a single row.
TEST_F(ScalarFunctionsTest, VectorizedOverMultipleRows) {
  duckdb_result r;
  ASSERT_EQ(duckdb_query(
                con_,
                "SELECT CAST(sqrt(v) AS INTEGER) FROM (VALUES (4.0), (NULL), "
                "(9.0), (16.0)) AS t(v) ORDER BY v NULLS FIRST",
                &r),
            DuckDBSuccess)
      << duckdb_result_error(&r);
  ASSERT_EQ(duckdb_row_count(&r), 4u);
  // NULLS FIRST -> row 0 is the NULL input -> NULL output; then 2, 3, 4.
  char* v0 = duckdb_value_varchar(&r, 0, 0);
  EXPECT_STREQ(v0 ? v0 : "NULL", "NULL");
  duckdb_free(v0);
  char* v1 = duckdb_value_varchar(&r, 0, 1);
  EXPECT_STREQ(v1 ? v1 : "NULL", "2");
  duckdb_free(v1);
  duckdb_destroy_result(&r);
}

}  // namespace
}  // namespace perfetto::trace_processor::duckdb_integration
