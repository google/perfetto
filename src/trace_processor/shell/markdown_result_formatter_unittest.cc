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

#include "src/trace_processor/shell/markdown_result_formatter.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/result_formatter.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using Row = std::vector<SqlValue>;

struct Formatted {
  std::string output;
  // Number of rows accepted before AddRow() asked to stop.
  size_t rows_read = 0;
};

Formatted Format(const ResultFormatOptions& options,
                 std::vector<std::string> columns,
                 const std::vector<Row>& rows) {
  FILE* file = tmpfile();
  PERFETTO_CHECK(file);
  Formatted res;
  {
    MarkdownResultFormatter formatter(options, std::move(columns), file);
    for (const Row& row : rows) {
      ++res.rows_read;
      if (!formatter.AddRow(row.data())) {
        break;
      }
    }
    formatter.Finish();
  }
  fseek(file, 0, SEEK_SET);
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof(buf), file)) > 0;) {
    res.output.append(buf, n);
  }
  fclose(file);
  return res;
}

std::vector<Row> NumberedRows(int64_t count) {
  std::vector<Row> rows;
  for (int64_t i = 0; i < count; ++i) {
    rows.push_back({SqlValue::Long(i)});
  }
  return rows;
}

ResultFormatOptions Markdown() {
  ResultFormatOptions options;
  options.format = ResultFormat::kMarkdown;
  return options;
}

TEST(MarkdownResultFormatterTest, Values) {
  auto res = Format(Markdown(), {"a", "b|c"},
                    {
                        {SqlValue::Long(1), SqlValue::String("x|y\nz")},
                        {SqlValue::Double(2), SqlValue()},
                        {SqlValue::Double(0.1), SqlValue::String("")},
                    });
  EXPECT_EQ(res.output, R"(| a | b\|c |
|---|---|
| 1 | x\|y\nz |
| 2.0 | NULL |
| 0.1 |  |
)");
}

TEST(MarkdownResultFormatterTest, Empty) {
  auto res = Format(Markdown(), {"a"}, {});
  EXPECT_EQ(res.output, R"(| a |
|---|

(0 rows)
)");
}

TEST(MarkdownResultFormatterTest, LongResultsHaveCount) {
  auto res = Format(Markdown(), {"a"}, NumberedRows(10));
  EXPECT_THAT(res.output, HasSubstr("| 9 |\n\n(10 rows)\n"));
  EXPECT_THAT(Format(Markdown(), {"a"}, NumberedRows(9)).output,
              Not(HasSubstr("rows")));
}

TEST(MarkdownResultFormatterTest, RowLimitShowsFirstAndLastRows) {
  ResultFormatOptions options = Markdown();
  options.max_rows = 1000;
  auto res = Format(options, {"a"}, NumberedRows(2000));
  EXPECT_EQ(res.rows_read, 2000u);
  EXPECT_THAT(res.output, HasSubstr("| 19 |\n| … 1960 rows omitted … |\n"
                                    "| 1980 |\n"));
  EXPECT_THAT(res.output, Not(HasSubstr("| 20 |")));
  EXPECT_THAT(res.output, HasSubstr("| 1999 |\n\n(first 20 and last 20 of "
                                    "2000 rows. Page with"));

  // Within the limit, everything is printed.
  EXPECT_THAT(Format(options, {"a"}, NumberedRows(1000)).output,
              Not(HasSubstr("omitted")));
}

TEST(MarkdownResultFormatterTest, SmallRowLimitShowsFewerRows) {
  ResultFormatOptions options = Markdown();
  options.max_rows = 4;
  auto res = Format(options, {"a"}, NumberedRows(10));
  EXPECT_THAT(res.output, HasSubstr(R"(|---|
| 0 |
| 1 |
| … 6 rows omitted … |
| 8 |
| 9 |
)"));
}

TEST(MarkdownResultFormatterTest, ByteLimitKeepsOutputSmall) {
  ResultFormatOptions options = Markdown();
  options.max_bytes = 1000;
  std::string wide(300, 'x');
  std::vector<Row> rows;
  for (int i = 0; i < 100; ++i) {
    rows.push_back({SqlValue::String(wide.c_str())});
  }
  auto res = Format(options, {"a"}, rows);
  // One row fits in each half of the budget.
  EXPECT_THAT(res.output, HasSubstr("| … 98 rows omitted … |"));
  EXPECT_THAT(res.output, HasSubstr("(first 1 and last 1 of 100 rows"));
}

TEST(MarkdownResultFormatterTest, StopsReadingFarPastTheLimit) {
  ResultFormatOptions options = Markdown();
  options.max_rows = 100;
  options.max_rows_past_limit = 50;
  auto res = Format(options, {"a"}, NumberedRows(100000));
  EXPECT_EQ(res.rows_read, 151u);
  EXPECT_THAT(res.output, HasSubstr("| 19 |\n| … |\n\n(first 20 of at least "
                                    "151 rows; stopped reading early."));
}

TEST(MarkdownResultFormatterTest, LongCellsAreCutOnCharacters) {
  ResultFormatOptions options = Markdown();
  options.max_cell_chars = 3;
  auto res =
      Format(options, {"a"},
             {{SqlValue::String("h\xC3\xA9llo")}, {SqlValue::String("abc")}});
  EXPECT_THAT(res.output, HasSubstr("| h\xC3\xA9l…(+2 chars) |\n| abc |\n"));
}

}  // namespace
}  // namespace perfetto::trace_processor
