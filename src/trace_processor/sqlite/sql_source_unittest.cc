/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/sqlite/sql_source.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(SqlSourceTest, Factory) {
  SqlSource source = SqlSource::FromExecuteQuery("SELECT * FROM slice");
  ASSERT_EQ(source.AsTraceback(0),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    SELECT * FROM slice\n"
            "    ^\n");
  ASSERT_EQ(source.AsTraceback(7),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    SELECT * FROM slice\n"
            "           ^\n");
}

TEST(SqlSourceTest, Substr) {
  SqlSource source =
      SqlSource::FromExecuteQuery("SELECT * FROM slice").Substr(9, 10);
  ASSERT_EQ(source.sql(), "FROM slice");

  ASSERT_EQ(source.AsTraceback(0),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 10\n"
            "    FROM slice\n"
            "    ^\n");
  ASSERT_EQ(source.AsTraceback(6),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 16\n"
            "    FROM slice\n"
            "          ^\n");
}

TEST(SqlSourceTest, RewriteAllIgnoreExisting) {
  SqlSource source =
      SqlSource::FromExecuteQuery("macro!()")
          .RewriteAllIgnoreExisting(SqlSource::FromTraceProcessorImplementation(
              "SELECT * FROM slice"));
  ASSERT_EQ(source.sql(), "SELECT * FROM slice");

  ASSERT_EQ(source.AsTraceback(0),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    macro!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 1\n"
            "    SELECT * FROM slice\n"
            "    ^\n");
  ASSERT_EQ(source.AsTraceback(7),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    macro!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 8\n"
            "    SELECT * FROM slice\n"
            "           ^\n");
}

TEST(SqlSourceTest, NestedFullRewrite) {
  SqlSource nested =
      SqlSource::FromTraceProcessorImplementation("nested!()")
          .RewriteAllIgnoreExisting(SqlSource::FromTraceProcessorImplementation(
              "SELECT * FROM slice"));
  ASSERT_EQ(nested.sql(), "SELECT * FROM slice");

  SqlSource source = SqlSource::FromExecuteQuery("macro!()")
                         .RewriteAllIgnoreExisting(std::move(nested));
  ASSERT_EQ(source.sql(), "SELECT * FROM slice");

  ASSERT_EQ(source.AsTraceback(0),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    macro!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 1\n"
            "    nested!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 1\n"
            "    SELECT * FROM slice\n"
            "    ^\n");
  ASSERT_EQ(source.AsTraceback(7),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    macro!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 1\n"
            "    nested!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 8\n"
            "    SELECT * FROM slice\n"
            "           ^\n");
}

TEST(SqlSourceTest, RewriteAllIgnoresExistingCorrectly) {
  SqlSource foo =
      SqlSource::FromExecuteQuery("foo!()").RewriteAllIgnoreExisting(
          SqlSource::FromTraceProcessorImplementation("SELECT * FROM slice"));
  SqlSource source = foo.RewriteAllIgnoreExisting(
      SqlSource::FromTraceProcessorImplementation("SELECT 0 WHERE 0"));
  ASSERT_EQ(source.sql(), "SELECT 0 WHERE 0");

  ASSERT_EQ(source.AsTraceback(0),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    foo!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 1\n"
            "    SELECT 0 WHERE 0\n"
            "    ^\n");
  ASSERT_EQ(source.AsTraceback(4),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    foo!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 5\n"
            "    SELECT 0 WHERE 0\n"
            "        ^\n");
}

TEST(SqlSourceTest, Rewriter) {
  SqlSource::Rewriter rewriter(
      SqlSource::FromExecuteQuery("SELECT cols!() FROM slice"));
  rewriter.Rewrite(7, 14,
                   SqlSource::FromTraceProcessorImplementation(
                       "ts, dur, ts + dur AS ts_end"));

  SqlSource rewritten = std::move(rewriter).Build();
  ASSERT_EQ(rewritten.sql(), "SELECT ts, dur, ts + dur AS ts_end FROM slice");

  // Offset points at the top level source.
  ASSERT_EQ(rewritten.AsTraceback(0),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    SELECT cols!() FROM slice\n"
            "    ^\n");
  ASSERT_EQ(rewritten.AsTraceback(40),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 21\n"
            "    SELECT cols!() FROM slice\n"
            "                        ^\n");

  // Offset points at the nested source.
  ASSERT_EQ(rewritten.AsTraceback(16),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    SELECT cols!() FROM slice\n"
            "           ^\n"
            "  Trace Processor Internal line 1 col 10\n"
            "    ts, dur, ts + dur AS ts_end\n"
            "             ^\n");
}

TEST(SqlSourceTest, NestedRewriter) {
  SqlSource::Rewriter nested_rewrite(
      SqlSource::FromTraceProcessorImplementation(
          "id, common_cols!(), other_cols!(), name"));
  nested_rewrite.Rewrite(
      4, 18, SqlSource::FromTraceProcessorImplementation("ts, dur"));
  nested_rewrite.Rewrite(20, 33,
                         SqlSource::FromTraceProcessorImplementation("depth"));

  SqlSource::Rewriter rewriter(
      SqlSource::FromExecuteQuery("SELECT cols!() FROM slice"));
  rewriter.Rewrite(7, 14, std::move(nested_rewrite).Build());

  SqlSource rewritten = std::move(rewriter).Build();
  ASSERT_EQ(rewritten.sql(), "SELECT id, ts, dur, depth, name FROM slice");

  // Offset points at the top level source.
  ASSERT_EQ(rewritten.AsTraceback(0),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    SELECT cols!() FROM slice\n"
            "    ^\n");
  ASSERT_EQ(rewritten.AsTraceback(37),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 21\n"
            "    SELECT cols!() FROM slice\n"
            "                        ^\n");

  // Offset points at the first nested source.
  ASSERT_EQ(rewritten.AsTraceback(15),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    SELECT cols!() FROM slice\n"
            "           ^\n"
            "  Trace Processor Internal line 1 col 5\n"
            "    id, common_cols!(), other_cols!(), name\n"
            "        ^\n"
            "  Trace Processor Internal line 1 col 5\n"
            "    ts, dur\n"
            "        ^\n");

  // Offset points at the second nested source.
  ASSERT_EQ(rewritten.AsTraceback(20),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    SELECT cols!() FROM slice\n"
            "           ^\n"
            "  Trace Processor Internal line 1 col 21\n"
            "    id, common_cols!(), other_cols!(), name\n"
            "                        ^\n"
            "  Trace Processor Internal line 1 col 1\n"
            "    depth\n"
            "    ^\n");
  ASSERT_EQ(rewritten.AsTraceback(22),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    SELECT cols!() FROM slice\n"
            "           ^\n"
            "  Trace Processor Internal line 1 col 21\n"
            "    id, common_cols!(), other_cols!(), name\n"
            "                        ^\n"
            "  Trace Processor Internal line 1 col 3\n"
            "    depth\n"
            "      ^\n");
}

TEST(SqlSourceTest, NestedRewriteSubstr) {
  SqlSource::Rewriter nested_rewrite(
      SqlSource::FromTraceProcessorImplementation(
          "id, common_cols!(), other_cols!(), name"));
  nested_rewrite.Rewrite(
      4, 18, SqlSource::FromTraceProcessorImplementation("ts, dur"));
  nested_rewrite.Rewrite(20, 33,
                         SqlSource::FromTraceProcessorImplementation("depth"));

  SqlSource::Rewriter rewriter(
      SqlSource::FromExecuteQuery("SELECT cols!() FROM slice"));
  rewriter.Rewrite(7, 14, std::move(nested_rewrite).Build());

  SqlSource rewritten = std::move(rewriter).Build();
  ASSERT_EQ(rewritten.sql(), "SELECT id, ts, dur, depth, name FROM slice");

  // Full macro cover.
  SqlSource cols = rewritten.Substr(7, 24);
  ASSERT_EQ(cols.sql(), "id, ts, dur, depth, name");
  ASSERT_EQ(cols.AsTraceback(0),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    cols!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 1\n"
            "    id, common_cols!(), other_cols!(), name\n"
            "    ^\n");
  ASSERT_EQ(cols.AsTraceback(5),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    cols!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 5\n"
            "    id, common_cols!(), other_cols!(), name\n"
            "        ^\n"
            "  Trace Processor Internal line 1 col 2\n"
            "    ts, dur\n"
            "     ^\n");
  ASSERT_EQ(cols.AsTraceback(14),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    cols!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 21\n"
            "    id, common_cols!(), other_cols!(), name\n"
            "                        ^\n"
            "  Trace Processor Internal line 1 col 2\n"
            "    depth\n"
            "     ^\n");

  // Intersect with nested.
  SqlSource intersect = rewritten.Substr(8, 13);
  ASSERT_EQ(intersect.sql(), "d, ts, dur, d");
  ASSERT_EQ(intersect.AsTraceback(0),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    cols!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 2\n"
            "    d, common_cols!(), other_cols!()\n"
            "    ^\n");
  ASSERT_EQ(intersect.AsTraceback(4),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    cols!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 5\n"
            "    d, common_cols!(), other_cols!()\n"
            "       ^\n"
            "  Trace Processor Internal line 1 col 2\n"
            "    ts, dur\n"
            "     ^\n");
  ASSERT_EQ(intersect.AsTraceback(12),
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 8\n"
            "    cols!()\n"
            "    ^\n"
            "  Trace Processor Internal line 1 col 21\n"
            "    d, common_cols!(), other_cols!()\n"
            "                       ^\n"
            "  Trace Processor Internal line 1 col 1\n"
            "    d\n"
            "    ^\n");
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
