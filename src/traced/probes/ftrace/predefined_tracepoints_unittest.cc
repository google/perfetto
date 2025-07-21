/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/traced/probes/ftrace/predefined_tracepoints.h"

#include <vector>

#include "test/gtest_and_gmock.h"

#include "src/traced/probes/ftrace/tracefs.h"

using testing::_;
using testing::ElementsAre;
using testing::Pair;
using testing::Return;

namespace perfetto::predefined_tracepoints {
namespace {
class MockTracefs : public Tracefs {
 public:
  MockTracefs() : Tracefs("/root/") {}
  MOCK_METHOD(bool, IsFileWriteable, (const std::string& path), (override));
  MOCK_METHOD(bool, IsFileReadable, (const std::string& path), (override));
};

class MockProtoTranslationTable : public ProtoTranslationTable {
 public:
  explicit MockProtoTranslationTable(const MockTracefs* ftrace)
      : ProtoTranslationTable(ftrace,
                              {},
                              {},
                              DefaultPageHeaderSpecForTesting(),
                              InvalidCompactSchedEventFormatForTesting(),
                              PrintkMap()) {}
  MOCK_METHOD(const std::vector<const Event*>*,
              GetEventsByGroup,
              (const std::string& group),
              (const, override));
};

TEST(PredefinedTracepointsTest, GetAccessiblePredefinedTracePoints) {
  MockTracefs ftrace;

  MockProtoTranslationTable table(&ftrace);
  Event unaccessible_proto_event{};
  unaccessible_proto_event.name = "unaccessible_proto_event";
  Event accessible_proto_event{};
  accessible_proto_event.name = "accessible_proto_event";

  std::vector<const Event*> proto_table_events(
      {&unaccessible_proto_event, &accessible_proto_event});

  EXPECT_CALL(table, GetEventsByGroup(_)).WillRepeatedly(Return(nullptr));
  // Add two events to the "gfx" category.
  EXPECT_CALL(table, GetEventsByGroup("mdss"))
      .WillOnce(Return(&proto_table_events));

  EXPECT_CALL(ftrace, IsFileWriteable(_)).WillRepeatedly(Return(false));
  EXPECT_CALL(ftrace, IsFileWriteable("/root/set_event"))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(ftrace, IsFileWriteable(
                          "/root/events/mdss/accessible_proto_event/enable"))
      .WillOnce(Return(true));
  // Enable the first and the second events from the 'freq' category.
  EXPECT_CALL(ftrace,
              IsFileWriteable("/root/events/power/cpu_frequency/enable"))
      .WillOnce(Return(true));
  EXPECT_CALL(ftrace,
              IsFileWriteable("/root/events/power/gpu_frequency/enable"))
      .WillOnce(Return(true));

  std::map<std::string, base::FlatSet<GroupAndName>> tracepoints =
      GetAccessiblePredefinedTracePoints(&table, &ftrace);

  EXPECT_THAT(
      tracepoints,
      ElementsAre(
          Pair("freq", ElementsAre(GroupAndName{"power", "cpu_frequency"},
                                   GroupAndName{"power", "gpu_frequency"})),
          Pair("gfx",
               ElementsAre(GroupAndName{"mdss", "accessible_proto_event"}))));
}

TEST(PredefinedTracepointsTest, GetAccessiblePredefinedTracePointsSetEvent) {
  MockTracefs ftrace;

  MockProtoTranslationTable table(&ftrace);
  Event unaccessible_proto_event{};
  unaccessible_proto_event.name = "unaccessible_proto_event";
  Event accessible_proto_event{};
  accessible_proto_event.name = "accessible_proto_event";

  std::vector<const Event*> proto_table_events(
      {&unaccessible_proto_event, &accessible_proto_event});

  EXPECT_CALL(table, GetEventsByGroup(_)).WillRepeatedly(Return(nullptr));
  // Add two events to the "gfx" category.
  EXPECT_CALL(table, GetEventsByGroup("mdss"))
      .WillOnce(Return(&proto_table_events));

  EXPECT_CALL(ftrace, IsFileWriteable(_)).WillRepeatedly(Return(false));
  EXPECT_CALL(ftrace, IsFileReadable(_)).WillRepeatedly(Return(false));
  EXPECT_CALL(ftrace, IsFileWriteable("/root/set_event"))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(ftrace,
              IsFileReadable("/root/events/mdss/accessible_proto_event/format"))
      .WillOnce(Return(true));
  // Enable the first and the second events from the 'freq' category.
  EXPECT_CALL(ftrace, IsFileReadable("/root/events/power/cpu_frequency/format"))
      .WillOnce(Return(true));
  EXPECT_CALL(ftrace, IsFileReadable("/root/events/power/gpu_frequency/format"))
      .WillOnce(Return(true));

  std::map<std::string, base::FlatSet<GroupAndName>> tracepoints =
      GetAccessiblePredefinedTracePoints(&table, &ftrace);

  EXPECT_THAT(
      tracepoints,
      ElementsAre(
          Pair("freq", ElementsAre(GroupAndName{"power", "cpu_frequency"},
                                   GroupAndName{"power", "gpu_frequency"})),
          Pair("gfx",
               ElementsAre(GroupAndName{"mdss", "accessible_proto_event"}))));
}

}  // namespace
}  // namespace perfetto::predefined_tracepoints
