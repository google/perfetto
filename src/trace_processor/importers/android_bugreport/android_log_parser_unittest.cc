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

#include "src/trace_processor/importers/android_bugreport/android_log_parser.h"

#include "perfetto/base/time.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/android_log_constants.pbzero.h"

namespace perfetto {
namespace trace_processor {

inline std::ostream& operator<<(std::ostream& stream,
                                const AndroidLogEvent& e) {
  char tms[32];
  time_t secs = static_cast<time_t>(e.ts / 1000000000);
  int ns = static_cast<int>(e.ts - secs * 1000000000);
  strftime(tms, sizeof(tms), "%Y-%m-%d %H:%M:%S", gmtime(&secs));
  base::StackString<64> tss("%s.%d", tms, ns);

  stream << "{ts=" << tss.c_str() << ", pid=" << e.pid << ", tid=" << e.tid
         << ", prio=" << e.prio << ", tag=" << e.tag.raw_id()
         << ", msg=" << e.msg.raw_id() << "}";
  return stream;
}

namespace {

using ::testing::ElementsAreArray;

TEST(AndroidLogParserTest, PersistentLogFormat) {
  TraceStorage storage;
  AndroidLogParser alp(2020, &storage);
  auto S = [&](const char* str) { return storage.InternString(str); };
  using P = ::perfetto::protos::pbzero::AndroidLogPriority;

  std::vector<AndroidLogEvent> events;
  alp.ParseLogLines(
      {
          "01-02 03:04:05.678901 1000 2000 D Tag: message",
          "01-02 03:04:05.678901 1000 2000 V Tag: message",
          "12-31 23:59:00.123456 1 2 I [tag:with:colon]: moar long message",
          "12-31 23:59:00.123 1 2 W [tag:with:colon]: moar long message",
          "12-31 23:59:00.1 1 2 E [tag:with:colon]: moar long message",
          "12-31 23:59:00.01 1 2 F [tag:with:colon]: moar long message",
      },
      &events);

  EXPECT_EQ(storage.stats()[stats::android_log_num_failed].value, 0);
  ASSERT_THAT(
      events,
      ElementsAreArray({
          AndroidLogEvent{
              base::MkTime(2020, 1, 2, 3, 4, 5) * 1000000000 + 678901000, 1000,
              2000, P::PRIO_DEBUG, S("Tag"), S("message")},
          AndroidLogEvent{
              base::MkTime(2020, 1, 2, 3, 4, 5) * 1000000000 + 678901000, 1000,
              2000, P::PRIO_VERBOSE, S("Tag"), S("message")},
          AndroidLogEvent{
              base::MkTime(2020, 12, 31, 23, 59, 0) * 1000000000 + 123456000, 1,
              2, P::PRIO_INFO, S("[tag:with:colon]"), S("moar long message")},
          AndroidLogEvent{
              base::MkTime(2020, 12, 31, 23, 59, 0) * 1000000000 + 123000000, 1,
              2, P::PRIO_WARN, S("[tag:with:colon]"), S("moar long message")},
          AndroidLogEvent{
              base::MkTime(2020, 12, 31, 23, 59, 0) * 1000000000 + 100000000, 1,
              2, P::PRIO_ERROR, S("[tag:with:colon]"), S("moar long message")},
          AndroidLogEvent{
              base::MkTime(2020, 12, 31, 23, 59, 0) * 1000000000 + 10000000, 1,
              2, P::PRIO_FATAL, S("[tag:with:colon]"), S("moar long message")},
      }));
}

TEST(AndroidLogParserTest, BugreportFormat) {
  TraceStorage storage;
  AndroidLogParser alp(2020, &storage);
  auto S = [&](const char* str) { return storage.InternString(str); };
  using P = ::perfetto::protos::pbzero::AndroidLogPriority;

  std::vector<AndroidLogEvent> events;
  alp.ParseLogLines(
      {
          "07-28 14:25:20.355  0     1     2 I init   : Loaded kernel module",
          "07-28 14:25:54.876  1000   643   644 D PackageManager: No files",
      },
      &events);

  EXPECT_EQ(storage.stats()[stats::android_log_num_failed].value, 0);
  ASSERT_THAT(
      events,
      ElementsAreArray({
          AndroidLogEvent{
              base::MkTime(2020, 7, 28, 14, 25, 20) * 1000000000 + 355000000, 1,
              2, P::PRIO_INFO, S("init"), S("Loaded kernel module")},
          AndroidLogEvent{
              base::MkTime(2020, 7, 28, 14, 25, 54) * 1000000000 + 876000000,
              643, 644, P::PRIO_DEBUG, S("PackageManager"), S("No files")},
      }));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
