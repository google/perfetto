/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/util/streaming_line_reader.h"

#include <algorithm>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/sys_types.h"  // For ssize_t on Windows.
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace util {

namespace {

using ::testing::ElementsAreArray;

struct LineSink {
  StreamingLineReader::LinesCallback AppendLinesCallback() {
    return [&](const std::vector<base::StringView>& lines_parsed) {
      for (const auto& sv : lines_parsed) {
        lines_.emplace_back(sv.ToStdString());
      }
    };
  }

  // Returns the lines received so far. Not idempotent, clears the vector.
  std::vector<std::string> GetLines() {
    auto lines = std::move(lines_);
    lines_.clear();
    return lines;
  }

  std::vector<std::string> lines_;
};

TEST(StreamingLineReaderTest, Tokenize) {
  LineSink sink;
  StreamingLineReader slr(sink.AppendLinesCallback());

  slr.Tokenize("a12\nb3456\nc\nd78\n\ne12\nf3456\n");
  ASSERT_THAT(sink.GetLines(), ElementsAreArray({"a12", "b3456", "c", "d78", "",
                                                 "e12", "f3456"}));
}

TEST(StreamingLineReaderTest, BeginEndWrite) {
  LineSink sink;
  StreamingLineReader slr(sink.AppendLinesCallback());

  char* w = slr.BeginWrite(9);
  slr.EndWrite(static_cast<size_t>(base::SprintfTrunc(w, 9, "a12\nb345")));
  ASSERT_THAT(sink.GetLines(), ElementsAreArray({"a12"}));

  w = slr.BeginWrite(9);
  slr.EndWrite(static_cast<size_t>(base::SprintfTrunc(w, 9, "6\nc\nd78\n")));
  ASSERT_THAT(sink.GetLines(), ElementsAreArray({"b3456", "c", "d78"}));

  w = slr.BeginWrite(4);  // Deliberately over-sizing the `reserve_size`.
  slr.EndWrite(static_cast<size_t>(base::SprintfTrunc(w, 4, "\n")));
  ASSERT_THAT(sink.GetLines(), ElementsAreArray({""}));

  w = slr.BeginWrite(128);  // Deliberately over-sizing the `reserve_size`.
  slr.EndWrite(static_cast<size_t>(base::SprintfTrunc(w, 128, "e12\nf3456\n")));
  ASSERT_THAT(sink.GetLines(), ElementsAreArray({"e12", "f3456"}));
}

// Creates a random text of 10000 chars which looks like the one below. Then
// feeds it into the SLR pushing chunks of random size. Checks that all the
// lines received match the original text.
TEST(StreamingLineReaderTest, RandomWrite) {
  LineSink sink;
  StreamingLineReader slr(sink.AppendLinesCallback());
  std::minstd_rand0 rnd(0);

  // Builds a random string with 10k char that looks like this:
  // geoefss1hmwgp9r6i3hlmpejjv6c4u2tsgbrwp30arkyb8b13ntek09f\n
  // t4q\n
  // \n
  // vr135li3m3330gy\n
  // ...
  std::string expected_txt(10000, '\0');
  static const char kRandChars[] = "\n0123456789abcdefghijklmnopqrstuvwxyz";
  for (size_t i = 0; i < expected_txt.size(); i++)
    expected_txt[i] = kRandChars[rnd() % strlen(kRandChars)];
  expected_txt[expected_txt.size() - 1] = '\n';

  // Push it in random chunks of max 1Kb.
  for (auto it = expected_txt.begin(); it < expected_txt.end();) {
    size_t wr_size = static_cast<size_t>(rnd()) % 1000ul;
    auto avail = static_cast<size_t>(std::distance(it, expected_txt.end()));
    wr_size = std::min(wr_size, avail);
    memcpy(slr.BeginWrite(wr_size), &*it, wr_size);
    slr.EndWrite(wr_size);
    it += static_cast<ssize_t>(wr_size);
  }

  // Merge the lines received and check they match the original text.
  std::string actual_txt;
  actual_txt.reserve(expected_txt.size());
  std::vector<std::string> lines = sink.GetLines();
  for (const std::string& line : lines) {
    actual_txt.append(line);
    actual_txt.append("\n");
  }

  ASSERT_EQ(actual_txt, expected_txt);
}

}  // namespace
}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto
