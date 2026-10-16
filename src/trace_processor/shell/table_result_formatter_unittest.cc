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

#include "src/trace_processor/shell/table_result_formatter.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/result_formatter.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using Row = std::vector<SqlValue>;

struct Formatted {
  std::string output;
  size_t prompts = 0;
};

// Answers the first |pages| prompts with "more", then "stop".
Formatted Format(uint64_t page_rows,
                 size_t pages,
                 std::vector<std::string> columns,
                 const std::vector<Row>& rows) {
  base::TempFile tmp = base::TempFile::Create().ValueOrDie();
  Formatted res;
  {
    base::ScopedFstream file = base::OpenFstream(tmp.path(), "w");
    PERFETTO_CHECK(file);
    TableResultFormatter formatter(
        DefaultTableOptions(page_rows), std::move(columns), *file,
        [&res, pages] { return ++res.prompts <= pages; });
    for (const Row& row : rows) {
      if (!formatter.AddRow(row.data())) {
        break;
      }
    }
    formatter.Finish();
  }
  PERFETTO_CHECK(base::ReadFile(tmp.path(), &res.output));
  return res;
}

std::vector<Row> NumberedRows(int64_t count) {
  std::vector<Row> rows;
  for (int64_t i = 0; i < count; ++i) {
    rows.push_back({SqlValue::Long(i)});
  }
  return rows;
}

TEST(TableResultFormatterTest, Values) {
  auto res = Format(
      0, 0, {"id", "name", "dur"},
      {
          {SqlValue::Long(1), SqlValue::String("a\nb"), SqlValue::Double(2)},
          {SqlValue::Long(100), SqlValue(), SqlValue::Double(0.5)},
      });
  EXPECT_EQ(res.output, R"( id  name  dur
---  ----  ---
  1  a\nb  2.0
100  NULL  0.5
)");
}

TEST(TableResultFormatterTest, StopsWhenAskedBetweenPages) {
  auto res = Format(2, 1, {"a"}, NumberedRows(10));
  EXPECT_EQ(res.prompts, 2u);
  EXPECT_EQ(res.output, R"(a
-
0
1
a
-
2
3
(stopped after 4 rows)
)");
}

TEST(TableResultFormatterTest, NoPromptWhenLastPageIsFull) {
  auto res = Format(2, 100, {"a"}, NumberedRows(4));
  EXPECT_EQ(res.prompts, 1u);
  EXPECT_EQ(res.output, "a\n-\n0\n1\na\n-\n2\n3\n");
}

}  // namespace
}  // namespace perfetto::trace_processor
