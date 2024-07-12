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

#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "src/trace_processor/importers/android_bugreport/android_bugreport_reader.h"
#include "src/trace_processor/importers/android_bugreport/android_log_event.h"
#include "src/trace_processor/importers/android_bugreport/android_log_reader.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/android_log_constants.pbzero.h"

namespace perfetto::trace_processor {

static void PrintTo(const AndroidLogEvent& event, std::ostream* os) {
  *os << "(pid: " << event.pid << ", "
      << "tid: " << event.tid << ", "
      << "prio: " << event.prio << ", "
      << "tag_id: " << event.tag.raw_id() << ", "
      << "msg_id: " << event.msg.raw_id() << ")";
}

namespace {
const int64_t kStoNs = 1000000000LL;

class EventParserMock : public AndroidLogEventParser {
 public:
  ~EventParserMock() override = default;
  MOCK_METHOD(void,
              ParseAndroidLogEvent,
              (int64_t, AndroidLogEvent),
              (override));
};

class AndroidLogReaderTest : public ::testing::Test {
 public:
  AndroidLogReaderTest() {
    context_.storage = std::make_shared<TraceStorage>();
    context_.clock_tracker = std::make_unique<ClockTracker>(&context_);
    context_.metadata_tracker =
        std::make_unique<MetadataTracker>(context_.storage.get());
    context_.clock_tracker->SetTraceTimeClock(
        protos::pbzero::ClockSnapshot::Clock::REALTIME);
    context_.sorter = std::make_unique<TraceSorter>(
        &context_, TraceSorter::SortingMode::kDefault);
    mock_parser_ = new EventParserMock();
    context_.android_log_event_parser.reset(mock_parser_);
  }

  using P = ::perfetto::protos::pbzero::AndroidLogPriority;

  StringId S(const char* str) { return context_.storage->InternString(str); }
  EventParserMock& mock_parser() { return *mock_parser_; }

  TraceProcessorContext* context() { return &context_; }

 private:
  TraceProcessorContext context_;
  EventParserMock* mock_parser_;
};

TEST_F(AndroidLogReaderTest, PersistentLogFormat) {
  constexpr char kInput[] =
      "01-02 03:04:05.678901 1000 2000 D Tag: message\n"
      "12-31 23:59:00.123456 1 2 I [tag:with:colon]: moar long message\n"
      "12-31 23:59:00.123 1 2 W [tag:with:colon]: moar long message\n"
      "12-31 23:59:00.1 1 2 E [tag:with:colon]: moar long message\n"
      "12-31 23:59:00.01 1 2 F [tag:with:colon]: moar long message\n";

  AndroidLogReader reader(context(), 2020);

  EXPECT_CALL(
      mock_parser(),
      ParseAndroidLogEvent(
          base::MkTime(2020, 1, 2, 3, 4, 5) * kStoNs + 678901000,
          AndroidLogEvent{1000, 2000, P::PRIO_DEBUG, S("Tag"), S("message")}));

  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 123456000,
                  AndroidLogEvent{1, 2, P::PRIO_INFO, S("[tag:with:colon]"),
                                  S("moar long message")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 123000000,
                  AndroidLogEvent{1, 2, P::PRIO_WARN, S("[tag:with:colon]"),
                                  S("moar long message")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 100000000,
                  AndroidLogEvent{1, 2, P::PRIO_ERROR, S("[tag:with:colon]"),
                                  S("moar long message")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 10000000,
                  AndroidLogEvent{1, 2, P::PRIO_FATAL, S("[tag:with:colon]"),
                                  S("moar long message")}));

  EXPECT_TRUE(
      reader.Parse(TraceBlobView(TraceBlob::CopyFrom(kInput, sizeof(kInput))))
          .ok());
  EXPECT_EQ(context()->storage->stats()[stats::android_log_num_failed].value,
            0);

  context()->sorter->ExtractEventsForced();
}

TEST_F(AndroidLogReaderTest, BugreportFormat) {
  constexpr char kInput[] =
      "07-28 14:25:20.355  0     1     2 I init   : Loaded kernel module\n"
      "07-28 14:25:54.876  1000   643   644 D PackageManager: No files\n"
      "08-24 23:39:12.272  root     0     1 I        : c0  11835 binder: 1\n"
      "08-24 23:39:12.421 radio  2532  2533 D TelephonyProvider: Using old\n";

  AndroidLogReader reader(context(), 2020);

  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 7, 28, 14, 25, 20) * kStoNs + 355000000,
                  AndroidLogEvent{1, 2, P::PRIO_INFO, S("init"),
                                  S("Loaded kernel module")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 7, 28, 14, 25, 54) * kStoNs + 876000000,
                  AndroidLogEvent{643, 644, P::PRIO_DEBUG, S("PackageManager"),
                                  S("No files")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 8, 24, 23, 39, 12) * kStoNs + 272000000,
                  AndroidLogEvent{0, 1, P::PRIO_INFO, S(""),
                                  S("c0  11835 binder: 1")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 8, 24, 23, 39, 12) * kStoNs + 421000000,
                  AndroidLogEvent{2532, 2533, P::PRIO_DEBUG,
                                  S("TelephonyProvider"), S("Using old")}));

  EXPECT_TRUE(
      reader.Parse(TraceBlobView(TraceBlob::CopyFrom(kInput, sizeof(kInput))))
          .ok());
  EXPECT_EQ(context()->storage->stats()[stats::android_log_num_failed].value,
            0);

  context()->sorter->ExtractEventsForced();
}

// Tests the deduping logic. This is used when parsing events first from the
// persistent logcat (which has us resolution) and then from dumpstate (which
// has ms resolution and sometimes contains dupes of the persistent entries).
TEST_F(AndroidLogReaderTest, Dedupe) {
  constexpr char kLogcatInput[] =
      "01-01 00:00:01.100000  0 1 1 I tag : M1\n"
      "01-01 00:00:01.100111  0 1 1 I tag : M2\n"
      "01-01 00:00:01.100111  0 1 1 I tag : M3\n"
      "01-01 00:00:01.100222  0 1 1 I tag : M4\n"
      "01-01 00:00:01.101000  0 1 1 I tag : M5\n";
  constexpr char kDumpstateInput[] =
      "01-01 00:00:01.100  0 1 1 I tag : M1\n"  // Dupe
      "01-01 00:00:01.100  0 1 1 I tag : M1\n"  // Not a dupe
      "01-01 00:00:01.100  0 1 1 I tag : M1.5\n"
      "01-01 00:00:01.100  0 1 1 I tag : M3\n"  // Dupe
      "01-01 00:00:01.100  0 1 1 I tag : M4\n"  // Dupe
      "01-01 00:00:01.101  0 1 1 I tag : M5\n"  // Dupe
      "01-01 00:00:01.101  0 1 1 I tag : M6\n";

  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100000000,
                  AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M1")}))
      .Times(2);
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100000000,
                  AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M1.5")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100111000,
                  AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M2")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100111000,
                  AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M3")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100222000,
                  AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M4")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 101000000,
                  AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M5")}));
  EXPECT_CALL(mock_parser(),
              ParseAndroidLogEvent(
                  base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 101000000,
                  AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M6")}));

  BufferingAndroidLogReader logcat_reader(context(), 2020);

  EXPECT_TRUE(logcat_reader
                  .Parse(TraceBlobView(
                      TraceBlob::CopyFrom(kLogcatInput, sizeof(kLogcatInput))))
                  .ok());

  DedupingAndroidLogReader dumstate_reader(
      context(), 2020, std::move(logcat_reader).ConsumeBufferedEvents());
  EXPECT_TRUE(dumstate_reader
                  .Parse(TraceBlobView(TraceBlob::CopyFrom(
                      kDumpstateInput, sizeof(kDumpstateInput))))
                  .ok());
  EXPECT_EQ(context()->storage->stats()[stats::android_log_num_failed].value,
            0);

  context()->sorter->ExtractEventsForced();
}

}  // namespace
}  // namespace perfetto::trace_processor
