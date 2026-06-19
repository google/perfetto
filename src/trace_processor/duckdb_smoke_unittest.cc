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

// Smoke test for the experimental DuckDB-backed query engine (M1). It only
// verifies that DuckDB is vendored, compiles, links and can execute a trivial
// query. It does not yet exercise any real trace_processor integration.

#include "duckdb.hpp"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(DuckDbSmokeTest, SelectFortyTwo) {
  duckdb::DuckDB db(nullptr);
  duckdb::Connection con(db);
  auto result = con.Query("SELECT 42");
  ASSERT_FALSE(result->HasError()) << result->GetError();
  ASSERT_EQ(result->RowCount(), 1u);
  ASSERT_EQ(result->ColumnCount(), 1u);
  EXPECT_EQ(result->GetValue(0, 0).GetValue<int64_t>(), 42);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
