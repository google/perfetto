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

const int64_t kStoNs = 1000000000LL;

inline std::ostream& operator<<(std::ostream& stream,
                                const AndroidLogEvent& e) {
  char tms[32];
  time_t secs = static_cast<time_t>(e.ts / kStoNs);
  int ns = static_cast<int>(e.ts - secs * kStoNs);
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
              base::MkTime(2020, 1, 2, 3, 4, 5) * kStoNs + 678901000, 1000,
              2000, P::PRIO_DEBUG, S("Tag"), S("message")},
          AndroidLogEvent{
              base::MkTime(2020, 1, 2, 3, 4, 5) * kStoNs + 678901000, 1000,
              2000, P::PRIO_VERBOSE, S("Tag"), S("message")},
          AndroidLogEvent{
              base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 123456000, 1, 2,
              P::PRIO_INFO, S("[tag:with:colon]"), S("moar long message")},
          AndroidLogEvent{
              base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 123000000, 1, 2,
              P::PRIO_WARN, S("[tag:with:colon]"), S("moar long message")},
          AndroidLogEvent{
              base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 100000000, 1, 2,
              P::PRIO_ERROR, S("[tag:with:colon]"), S("moar long message")},
          AndroidLogEvent{
              base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 10000000, 1, 2,
              P::PRIO_FATAL, S("[tag:with:colon]"), S("moar long message")},
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
          "08-24 23:39:12.272  root     0     1 I        : c0  11835 binder: 1",
          "08-24 23:39:12.421 radio  2532  2533 D TelephonyProvider: Using old",
      },
      &events);

  EXPECT_EQ(storage.stats()[stats::android_log_num_failed].value, 0);
  ASSERT_THAT(
      events,
      ElementsAreArray({
          AndroidLogEvent{
              base::MkTime(2020, 7, 28, 14, 25, 20) * kStoNs + 355000000, 1, 2,
              P::PRIO_INFO, S("init"), S("Loaded kernel module")},
          AndroidLogEvent{
              base::MkTime(2020, 7, 28, 14, 25, 54) * kStoNs + 876000000, 643,
              644, P::PRIO_DEBUG, S("PackageManager"), S("No files")},
          AndroidLogEvent{
              base::MkTime(2020, 8, 24, 23, 39, 12) * kStoNs + 272000000, 0, 1,
              P::PRIO_INFO, S(""), S("c0  11835 binder: 1")},
          AndroidLogEvent{
              base::MkTime(2020, 8, 24, 23, 39, 12) * kStoNs + 421000000, 2532,
              2533, P::PRIO_DEBUG, S("TelephonyProvider"), S("Using old")},
      }));
}

// Tests the deduping logic. This is used when parsing events first from the
// persistent logcat (which has us resolution) and then from dumpstate (which
// has ms resolution and sometimes contains dupes of the persistent entries).
TEST(AndroidLogParserTest, Dedupe) {
  TraceStorage storage;
  AndroidLogParser alp(2020, &storage);
  auto S = [&](const char* str) { return storage.InternString(str); };
  using P = ::perfetto::protos::pbzero::AndroidLogPriority;
  std::vector<AndroidLogEvent> events;

  // Parse some initial events without any deduping.
  alp.ParseLogLines(
      {
          "01-01 00:00:01.100000  0 1 1 I tag : M1",
          "01-01 00:00:01.100111  0 1 1 I tag : M2",
          "01-01 00:00:01.100111  0 1 1 I tag : M3",
          "01-01 00:00:01.100222  0 1 1 I tag : M4",
          "01-01 00:00:01.101000  0 1 1 I tag : M5",
      },
      &events);

  ASSERT_EQ(events.size(), 5u);

  // Add a batch of events with truncated timestamps, some of which are dupes.
  alp.ParseLogLines(
      {
          "01-01 00:00:01.100  0 1 1 I tag : M1",  // Dupe
          "01-01 00:00:01.100  0 1 1 I tag : M1.5",
          "01-01 00:00:01.100  0 1 1 I tag : M3",  // Dupe
          "01-01 00:00:01.100  0 1 1 I tag : M4",  // Dupe
          "01-01 00:00:01.101  0 1 1 I tag : M5",  // Dupe
          "01-01 00:00:01.101  0 1 1 I tag : M6",
      },
      &events, /*dedupe_idx=*/5);
  EXPECT_EQ(storage.stats()[stats::android_log_num_failed].value, 0);

  std::stable_sort(events.begin(), events.end());
  ASSERT_THAT(events,
              ElementsAreArray({
                  AndroidLogEvent{
                      base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100000000, 1,
                      1, P::PRIO_INFO, S("tag"), S("M1")},
                  AndroidLogEvent{
                      base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100000000, 1,
                      1, P::PRIO_INFO, S("tag"), S("M1.5")},
                  AndroidLogEvent{
                      base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100111000, 1,
                      1, P::PRIO_INFO, S("tag"), S("M2")},
                  AndroidLogEvent{
                      base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100111000, 1,
                      1, P::PRIO_INFO, S("tag"), S("M3")},
                  AndroidLogEvent{
                      base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100222000, 1,
                      1, P::PRIO_INFO, S("tag"), S("M4")},
                  AndroidLogEvent{
                      base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 101000000, 1,
                      1, P::PRIO_INFO, S("tag"), S("M5")},
                  AndroidLogEvent{
                      base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 101000000, 1,
                      1, P::PRIO_INFO, S("tag"), S("M6")},
              }));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
