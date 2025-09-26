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

#include "src/trace_processor/importers/android_bugreport/android_log_reader.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "src/trace_processor/importers/android_bugreport/android_bugreport_reader.h"
#include "src/trace_processor/importers/android_bugreport/android_log_event.h"
#include "src/trace_processor/importers/android_bugreport/android_log_event_parser.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
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

class EventParserMock
    : public TraceSorter::Sink<AndroidLogEvent, EventParserMock> {
 public:
  ~EventParserMock() override = default;
  MOCK_METHOD(void, Parse, (int64_t, AndroidLogEvent));
};

class AndroidLogReaderTest : public ::testing::Test {
 public:
  AndroidLogReaderTest() {
    context_.storage = std::make_unique<TraceStorage>();
    std::unique_ptr<ClockSynchronizerListenerImpl> clock_tracker_listener =
        std::make_unique<ClockSynchronizerListenerImpl>(&context_);
    context_.clock_tracker =
        std::make_unique<ClockTracker>(std::move(clock_tracker_listener));
    context_.metadata_tracker =
        std::make_unique<MetadataTracker>(context_.storage.get());
    context_.clock_tracker->SetTraceTimeClock(
        protos::pbzero::ClockSnapshot::Clock::REALTIME);
    context_.sorter = std::make_unique<TraceSorter>(
        &context_, TraceSorter::SortingMode::kDefault);
  }

  using P = ::perfetto::protos::pbzero::AndroidLogPriority;

  StringId S(const char* str) { return context_.storage->InternString(str); }

  TraceProcessorContext* context() { return &context_; }

 private:
  TraceProcessorContext context_;
};

TEST_F(AndroidLogReaderTest, PersistentLogFormat) {
  constexpr char kInput[] =
      "01-02 03:04:05.678901 1000 2000 D Tag: message\n"
      "12-31 23:59:00.123456 1 2 I [tag:with:colon]: moar long message\n"
      "12-31 23:59:00.123 1 2 W [tag:with:colon]: moar long message\n"
      "12-31 23:59:00.1 1 2 E [tag:with:colon]: moar long message\n"
      "12-31 23:59:00.01 1 2 F [tag:with:colon]: moar long message\n";

  auto mock_parser = std::make_unique<EventParserMock>();
  auto* mock_parser_ptr = mock_parser.get();
  AndroidLogReader reader(
      context(), 2020, context()->sorter->CreateStream(std::move(mock_parser)));

  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 1, 2, 3, 4, 5) * kStoNs + 678901000,
                    AndroidLogEvent{1000, 2000, P::PRIO_DEBUG, S("Tag"),
                                    S("message")}));

  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 123456000,
                    AndroidLogEvent{1, 2, P::PRIO_INFO, S("[tag:with:colon]"),
                                    S("moar long message")}));
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 123000000,
                    AndroidLogEvent{1, 2, P::PRIO_WARN, S("[tag:with:colon]"),
                                    S("moar long message")}));
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 100000000,
                    AndroidLogEvent{1, 2, P::PRIO_ERROR, S("[tag:with:colon]"),
                                    S("moar long message")}));
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 12, 31, 23, 59, 0) * kStoNs + 10000000,
                    AndroidLogEvent{1, 2, P::PRIO_FATAL, S("[tag:with:colon]"),
                                    S("moar long message")}));

  EXPECT_TRUE(
      reader.Parse(TraceBlobView(TraceBlob::CopyFrom(kInput, sizeof(kInput))))
          .ok());
  EXPECT_EQ(context()->storage->stats()[stats::android_log_num_failed].value,
            0);

  context()->sorter->ExtractEventsForced();
}

TEST_F(AndroidLogReaderTest, PersistentLogFormatWithYear) {
  constexpr char kInput[] =
      "2023-01-02 03:04:05.678901 1000 2000 D Tag: message\n"
      "2024-12-31 23:59:00.123456 1 2 I [tag:with:colon]: moar long message\n"
      "2025-06-15 12:30:45.987654 3 4 W SomeTag: warning message\n"
      "2023-03-14 09:26:53.500000 5 6 E ErrorTag: error occurred\n"
      "2024-07-04 16:20:30.250000 7 8 F FatalTag: fatal error\n";

  auto mock_parser = std::make_unique<EventParserMock>();
  auto* mock_parser_ptr = mock_parser.get();
  AndroidLogReader reader(
      context(), 2020, context()->sorter->CreateStream(std::move(mock_parser)));

  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2023, 1, 2, 3, 4, 5) * kStoNs + 678901000,
                    AndroidLogEvent{1000, 2000, P::PRIO_DEBUG, S("Tag"),
                                    S("message")}));

  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2024, 12, 31, 23, 59, 0) * kStoNs + 123456000,
                    AndroidLogEvent{1, 2, P::PRIO_INFO, S("[tag:with:colon]"),
                                    S("moar long message")}));
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2025, 6, 15, 12, 30, 45) * kStoNs + 987654000,
                    AndroidLogEvent{3, 4, P::PRIO_WARN, S("SomeTag"),
                                    S("warning message")}));
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2023, 3, 14, 9, 26, 53) * kStoNs + 500000000,
                    AndroidLogEvent{5, 6, P::PRIO_ERROR, S("ErrorTag"),
                                    S("error occurred")}));
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2024, 7, 4, 16, 20, 30) * kStoNs + 250000000,
                    AndroidLogEvent{7, 8, P::PRIO_FATAL, S("FatalTag"),
                                    S("fatal error")}));

  EXPECT_TRUE(
      reader.Parse(TraceBlobView(TraceBlob::CopyFrom(kInput, sizeof(kInput))))
          .ok());
  EXPECT_EQ(context()->storage->stats()[stats::android_log_num_failed].value,
            0);

  context()->sorter->ExtractEventsForced();
}

TEST_F(AndroidLogReaderTest, MixedDateFormats) {
  constexpr char kInput[] =
      "2023-01-02 03:04:05.678901 1000 2000 D Tag: with year\n"
      "01-15 12:30:45.987654 3 4 W SomeTag: without year\n"
      "2024-12-31 23:59:00.123456 1 2 I [tag:with:colon]: with year again\n"
      "06-15 09:26:53.500000 5 6 E ErrorTag: without year again\n";

  auto mock_parser = std::make_unique<EventParserMock>();
  auto* mock_parser_ptr = mock_parser.get();
  AndroidLogReader reader(
      context(), 2020, context()->sorter->CreateStream(std::move(mock_parser)));

  // Lines with year should use the parsed year (2023, 2024)
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2023, 1, 2, 3, 4, 5) * kStoNs + 678901000,
                    AndroidLogEvent{1000, 2000, P::PRIO_DEBUG, S("Tag"),
                                    S("with year")}));

  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2024, 12, 31, 23, 59, 0) * kStoNs + 123456000,
                    AndroidLogEvent{1, 2, P::PRIO_INFO, S("[tag:with:colon]"),
                                    S("with year again")}));

  // Lines without year should use the fallback year (2020)
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 1, 15, 12, 30, 45) * kStoNs + 987654000,
                    AndroidLogEvent{3, 4, P::PRIO_WARN, S("SomeTag"),
                                    S("without year")}));

  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 6, 15, 9, 26, 53) * kStoNs + 500000000,
                    AndroidLogEvent{5, 6, P::PRIO_ERROR, S("ErrorTag"),
                                    S("without year again")}));

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

  auto mock_parser = std::make_unique<EventParserMock>();
  auto* mock_parser_ptr = mock_parser.get();
  AndroidLogReader reader(
      context(), 2020, context()->sorter->CreateStream(std::move(mock_parser)));

  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 7, 28, 14, 25, 20) * kStoNs + 355000000,
                    AndroidLogEvent{1, 2, P::PRIO_INFO, S("init"),
                                    S("Loaded kernel module")}));
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 7, 28, 14, 25, 54) * kStoNs + 876000000,
                    AndroidLogEvent{643, 644, P::PRIO_DEBUG,
                                    S("PackageManager"), S("No files")}));
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 8, 24, 23, 39, 12) * kStoNs + 272000000,
                    AndroidLogEvent{0, 1, P::PRIO_INFO, S(""),
                                    S("c0  11835 binder: 1")}));
  EXPECT_CALL(*mock_parser_ptr,
              Parse(base::MkTime(2020, 8, 24, 23, 39, 12) * kStoNs + 421000000,
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

  auto mock_parser_for_logcat = std::make_unique<EventParserMock>();
  auto* mock_parser_for_logcat_ptr = mock_parser_for_logcat.get();
  BufferingAndroidLogReader logcat_reader(
      context(), 2020,
      context()->sorter->CreateStream(std::move(mock_parser_for_logcat)));

  EXPECT_TRUE(logcat_reader
                  .Parse(TraceBlobView(
                      TraceBlob::CopyFrom(kLogcatInput, sizeof(kLogcatInput))))
                  .ok());

  auto mock_parser_for_dumpstate = std::make_unique<EventParserMock>();
  auto* mock_parser_for_dumpstate_ptr = mock_parser_for_dumpstate.get();
  DedupingAndroidLogReader dumstate_reader(
      context(), 2020,
      context()->sorter->CreateStream(std::move(mock_parser_for_dumpstate)),
      false, std::move(logcat_reader).ConsumeBufferedEvents());

  EXPECT_CALL(*mock_parser_for_logcat_ptr,
              Parse(base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100000000,
                    AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M1")}));
  EXPECT_CALL(*mock_parser_for_logcat_ptr,
              Parse(base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100111000,
                    AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M2")}));
  EXPECT_CALL(*mock_parser_for_logcat_ptr,
              Parse(base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100111000,
                    AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M3")}));
  EXPECT_CALL(*mock_parser_for_logcat_ptr,
              Parse(base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100222000,
                    AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M4")}));
  EXPECT_CALL(*mock_parser_for_logcat_ptr,
              Parse(base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 101000000,
                    AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M5")}));

  EXPECT_CALL(*mock_parser_for_dumpstate_ptr,
              Parse(base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100000000,
                    AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M1")}));
  EXPECT_CALL(*mock_parser_for_dumpstate_ptr,
              Parse(base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 100000000,
                    AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M1.5")}));
  EXPECT_CALL(*mock_parser_for_dumpstate_ptr,
              Parse(base::MkTime(2020, 1, 1, 0, 0, 1) * kStoNs + 101000000,
                    AndroidLogEvent{1, 1, P::PRIO_INFO, S("tag"), S("M6")}));
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
