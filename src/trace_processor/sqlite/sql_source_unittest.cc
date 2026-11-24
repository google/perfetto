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
            R"( --> query:1:1
  |
1 | SELECT * FROM slice
  | ^
)");
  ASSERT_EQ(source.AsTraceback(7),
            R"( --> query:1:8
  |
1 | SELECT * FROM slice
  |        ^
)");
}

TEST(SqlSourceTest, Substr) {
  SqlSource source =
      SqlSource::FromExecuteQuery("SELECT * FROM slice").Substr(9, 10);
  ASSERT_EQ(source.sql(), "FROM slice");

  ASSERT_EQ(source.AsTraceback(0),
            R"( --> query:1:10
  |
1 | FROM slice
  | ^
)");
  ASSERT_EQ(source.AsTraceback(6),
            R"( --> query:1:16
  |
1 | FROM slice
  |       ^
)");
}

TEST(SqlSourceTest, RewriteAllIgnoreExisting) {
  SqlSource source =
      SqlSource::FromExecuteQuery("macro!()")
          .RewriteAllIgnoreExisting(SqlSource::FromTraceProcessorImplementation(
              "SELECT * FROM slice"));
  ASSERT_EQ(source.sql(), "SELECT * FROM slice");

  ASSERT_EQ(source.AsTraceback(0),
            R"( --> query:1:1
  |
1 | macro!()
  | ^
  = note: fully expanded statement:
          SELECT * FROM slice
          ^
 --> trace processor internal:1:1
  |
1 | SELECT * FROM slice
  | ^
)");
  ASSERT_EQ(source.AsTraceback(7),
            R"( --> query:1:1
  |
1 | macro!()
  | ^
  = note: fully expanded statement:
          SELECT * FROM slice
                 ^
 --> trace processor internal:1:8
  |
1 | SELECT * FROM slice
  |        ^
)");
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
            R"( --> query:1:1
  |
1 | macro!()
  | ^
  = note: fully expanded statement:
          SELECT * FROM slice
          ^
 --> trace processor internal:1:1
  |
1 | nested!()
  | ^
 --> trace processor internal:1:1
  |
1 | SELECT * FROM slice
  | ^
)");
  ASSERT_EQ(source.AsTraceback(7),
            R"( --> query:1:1
  |
1 | macro!()
  | ^
  = note: fully expanded statement:
          SELECT * FROM slice
                 ^
 --> trace processor internal:1:1
  |
1 | nested!()
  | ^
 --> trace processor internal:1:8
  |
1 | SELECT * FROM slice
  |        ^
)");
}

TEST(SqlSourceTest, RewriteAllIgnoresExistingCorrectly) {
  SqlSource foo =
      SqlSource::FromExecuteQuery("foo!()").RewriteAllIgnoreExisting(
          SqlSource::FromTraceProcessorImplementation("SELECT * FROM slice"));
  SqlSource source = foo.RewriteAllIgnoreExisting(
      SqlSource::FromTraceProcessorImplementation("SELECT 0 WHERE 0"));
  ASSERT_EQ(source.sql(), "SELECT 0 WHERE 0");

  ASSERT_EQ(source.AsTraceback(0),
            R"( --> query:1:1
  |
1 | foo!()
  | ^
  = note: fully expanded statement:
          SELECT 0 WHERE 0
          ^
 --> trace processor internal:1:1
  |
1 | SELECT 0 WHERE 0
  | ^
)");
  ASSERT_EQ(source.AsTraceback(4),
            R"( --> query:1:1
  |
1 | foo!()
  | ^
  = note: fully expanded statement:
          SELECT 0 WHERE 0
              ^
 --> trace processor internal:1:5
  |
1 | SELECT 0 WHERE 0
  |     ^
)");
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
            R"( --> query:1:1
  |
1 | SELECT cols!() FROM slice
  | ^
  = note: fully expanded statement:
          SELECT ts, dur, ts + dur AS ts_end FROM slice
          ^
)");
  ASSERT_EQ(rewritten.AsTraceback(40),
            R"( --> query:1:21
  |
1 | SELECT cols!() FROM slice
  |                     ^
  = note: fully expanded statement:
          SELECT ts, dur, ts + dur AS ts_end FROM slice
                                                  ^
)");

  // Offset points at the nested source.
  ASSERT_EQ(rewritten.AsTraceback(16),
            R"( --> query:1:8
  |
1 | SELECT cols!() FROM slice
  |        ^
  = note: fully expanded statement:
          SELECT ts, dur, ts + dur AS ts_end FROM slice
                          ^
 --> trace processor internal:1:10
  |
1 | ts, dur, ts + dur AS ts_end
  |          ^
)");
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
            R"( --> query:1:1
  |
1 | SELECT cols!() FROM slice
  | ^
  = note: fully expanded statement:
          SELECT id, ts, dur, depth, name FROM slice
          ^
)");
  ASSERT_EQ(rewritten.AsTraceback(37),
            R"( --> query:1:21
  |
1 | SELECT cols!() FROM slice
  |                     ^
  = note: fully expanded statement:
          SELECT id, ts, dur, depth, name FROM slice
                                               ^
)");

  // Offset points at the first nested source.
  ASSERT_EQ(rewritten.AsTraceback(15),
            R"( --> query:1:8
  |
1 | SELECT cols!() FROM slice
  |        ^
  = note: fully expanded statement:
          SELECT id, ts, dur, depth, name FROM slice
                         ^
 --> trace processor internal:1:5
  |
1 | id, common_cols!(), other_cols!(), name
  |     ^
 --> trace processor internal:1:5
  |
1 | ts, dur
  |     ^
)");

  // Offset points at the second nested source.
  ASSERT_EQ(rewritten.AsTraceback(20),
            R"( --> query:1:8
  |
1 | SELECT cols!() FROM slice
  |        ^
  = note: fully expanded statement:
          SELECT id, ts, dur, depth, name FROM slice
                              ^
 --> trace processor internal:1:21
  |
1 | id, common_cols!(), other_cols!(), name
  |                     ^
 --> trace processor internal:1:1
  |
1 | depth
  | ^
)");
  ASSERT_EQ(rewritten.AsTraceback(22),
            R"( --> query:1:8
  |
1 | SELECT cols!() FROM slice
  |        ^
  = note: fully expanded statement:
          SELECT id, ts, dur, depth, name FROM slice
                                ^
 --> trace processor internal:1:21
  |
1 | id, common_cols!(), other_cols!(), name
  |                     ^
 --> trace processor internal:1:3
  |
1 | depth
  |   ^
)");
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
            R"( --> query:1:8
  |
1 | cols!()
  | ^
  = note: fully expanded statement:
          id, ts, dur, depth, name
          ^
 --> trace processor internal:1:1
  |
1 | id, common_cols!(), other_cols!(), name
  | ^
)");
  ASSERT_EQ(cols.AsTraceback(5),
            R"( --> query:1:8
  |
1 | cols!()
  | ^
  = note: fully expanded statement:
          id, ts, dur, depth, name
               ^
 --> trace processor internal:1:5
  |
1 | id, common_cols!(), other_cols!(), name
  |     ^
 --> trace processor internal:1:2
  |
1 | ts, dur
  |  ^
)");
  ASSERT_EQ(cols.AsTraceback(14),
            R"( --> query:1:8
  |
1 | cols!()
  | ^
  = note: fully expanded statement:
          id, ts, dur, depth, name
                        ^
 --> trace processor internal:1:21
  |
1 | id, common_cols!(), other_cols!(), name
  |                     ^
 --> trace processor internal:1:2
  |
1 | depth
  |  ^
)");

  // Intersect with nested.
  SqlSource intersect = rewritten.Substr(8, 13);
  ASSERT_EQ(intersect.sql(), "d, ts, dur, d");
  ASSERT_EQ(intersect.AsTraceback(0),
            R"( --> query:1:8
  |
1 | cols!()
  | ^
  = note: fully expanded statement:
          d, ts, dur, d
          ^
 --> trace processor internal:1:2
  |
1 | d, common_cols!(), other_cols!()
  | ^
)");
  ASSERT_EQ(intersect.AsTraceback(4),
            R"( --> query:1:8
  |
1 | cols!()
  | ^
  = note: fully expanded statement:
          d, ts, dur, d
              ^
 --> trace processor internal:1:5
  |
1 | d, common_cols!(), other_cols!()
  |    ^
 --> trace processor internal:1:2
  |
1 | ts, dur
  |  ^
)");
  ASSERT_EQ(intersect.AsTraceback(12),
            R"( --> query:1:8
  |
1 | cols!()
  | ^
  = note: fully expanded statement:
          d, ts, dur, d
                      ^
 --> trace processor internal:1:21
  |
1 | d, common_cols!(), other_cols!()
  |                    ^
 --> trace processor internal:1:1
  |
1 | d
  | ^
)");
}

TEST(SqlSourceTest, Rerewrites) {
  SqlSource::Rewriter rewriter(
      SqlSource::FromExecuteQuery("SELECT foo!(a) FROM bar!(slice) a"));
  rewriter.Rewrite(7, 14,
                   SqlSource::FromTraceProcessorImplementation("a.x, a.y"));
  rewriter.Rewrite(20, 31,
                   SqlSource::FromTraceProcessorImplementation(
                       "(SELECT slice.x, slice.y, slice.z FROM slice)"));

  SqlSource rewritten = std::move(rewriter).Build();
  ASSERT_EQ(
      rewritten.sql(),
      "SELECT a.x, a.y FROM (SELECT slice.x, slice.y, slice.z FROM slice) a");

  SqlSource::Rewriter rerewriter(std::move(rewritten));
  rerewriter.Rewrite(0, 7,
                     SqlSource::FromTraceProcessorImplementation("INSERT "));
  rerewriter.Rewrite(7, 14,
                     SqlSource::FromTraceProcessorImplementation("a.z, "));

  SqlSource rerewritten = std::move(rerewriter).Build();
  ASSERT_EQ(
      rerewritten.sql(),
      "INSERT a.z, y FROM (SELECT slice.x, slice.y, slice.z FROM slice) a");
  ASSERT_EQ(rerewritten.AsTraceback(0),
            R"( --> query:1:1
  |
1 | SELECT foo!(a) FROM bar!(slice) a
  | ^
  = note: fully expanded statement:
          INSERT a.z, y FROM (SELECT slice.x, slice.y, slice.z FROM slice) a
          ^
 --> trace processor internal:1:1
  |
1 | INSERT 
  | ^
)");
  ASSERT_EQ(rerewritten.AsTraceback(8),
            R"( --> query:1:8
  |
1 | SELECT foo!(a) FROM bar!(slice) a
  |        ^
  = note: fully expanded statement:
          INSERT a.z, y FROM (SELECT slice.x, slice.y, slice.z FROM slice) a
                  ^
 --> trace processor internal:1:1
  |
1 | a.x, a.y
  | ^
 --> trace processor internal:1:2
  |
1 | a.z, 
  |  ^
)");
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
