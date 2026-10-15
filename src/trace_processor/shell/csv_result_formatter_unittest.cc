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

#include "src/trace_processor/shell/csv_result_formatter.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/result_formatter.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::HasSubstr;
using Row = std::vector<SqlValue>;

struct Formatted {
  std::string output;
  std::string notes;
  // Number of rows accepted before AddRow() asked to stop.
  size_t rows_read = 0;
};

std::string ReadAll(FILE* file) {
  std::string res;
  fseek(file, 0, SEEK_SET);
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof(buf), file)) > 0;) {
    res.append(buf, n);
  }
  fclose(file);
  return res;
}

Formatted Format(const ResultFormatOptions& options,
                 std::vector<std::string> columns,
                 const std::vector<Row>& rows) {
  FILE* output = tmpfile();
  FILE* notes = tmpfile();
  PERFETTO_CHECK(output && notes);
  Formatted res;
  {
    CsvResultFormatter formatter(options, std::move(columns), output, notes);
    for (const Row& row : rows) {
      ++res.rows_read;
      if (!formatter.AddRow(row.data())) {
        break;
      }
    }
    formatter.Finish();
  }
  res.output = ReadAll(output);
  res.notes = ReadAll(notes);
  return res;
}

std::vector<Row> NumberedRows(int64_t count) {
  std::vector<Row> rows;
  for (int64_t i = 0; i < count; ++i) {
    rows.push_back({SqlValue::Long(i)});
  }
  return rows;
}

TEST(CsvResultFormatterTest, Values) {
  auto res = Format({}, {"a", "b"},
                    {
                        {SqlValue::Long(1), SqlValue::String("x")},
                        {SqlValue::Double(2), SqlValue()},
                    });
  EXPECT_EQ(res.output, R"("a","b"
1,"x"
2.000000,"[NULL]"
)");
  EXPECT_EQ(res.notes, "");
}

TEST(CsvResultFormatterTest, RowLimitKeepsFirstRows) {
  ResultFormatOptions options;
  options.max_rows = 2;
  auto res = Format(options, {"a"}, NumberedRows(10));
  EXPECT_EQ(res.rows_read, 10u);
  EXPECT_EQ(res.output, "\"a\"\n0\n1\n");
  EXPECT_THAT(res.notes, HasSubstr("(first 2 of 10 rows."));
}

TEST(CsvResultFormatterTest, ByteLimitAlwaysKeepsOneRow) {
  ResultFormatOptions options;
  options.max_bytes = 1;
  auto res = Format(options, {"a"}, NumberedRows(3));
  EXPECT_EQ(res.output, "\"a\"\n0\n");
  EXPECT_THAT(res.notes, HasSubstr("(first 1 of 3 rows."));
}

}  // namespace
}  // namespace perfetto::trace_processor
