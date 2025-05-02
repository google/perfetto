/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/drop_empty_ftrace_events.h"
#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace_packet.gen.h"

// After other transformations are done running, empty ftrace packets may be
// left behind. A ftrace packet is considered empty when there is no more than
// a pid and timestamp.
//
// If an event (ftrace packet) is empty, it should be removed from the ftrace
// list (ftrace_events). If ftrace_events is empty (only contains a cpu), it
// should be removed from the packet.
//
//  packet: {
//    ftrace_events: {
//      cpu  : 0x00000003
//      event: {
//        timestamp : 0x0000001d5d0ce35d
//        pid       : 0x00400005
//      }
//      event: {
//        timestamp : 0x0000001d5d0d7314
//        pid       : 0x00400005
//      }
//    }
//  }

namespace perfetto::trace_redaction {
namespace {
constexpr auto kPid = 1u;
constexpr auto kTimes = std::array<uint64_t, 2>{1000, 2000};
}  // namespace

// Each event has a payload (print message) and should not be dropped.
//
//  packet: {
//    ftrace_events: {
//      cpu  : 0x00000003
//      event: {
//        timestamp : 0x0000001d5d0ce35d
//        pid       : 0x00400005
//        print     : {
//          buf: "TEXT A"
//        }
//      }
//      event: {
//        timestamp : 0x0000001d5d0d7314
//        pid       : 0x00400005
//        print     : {
//          buf: "TEXT B"
//        }
//      }
//    }
//  }
TEST(DropEmptyFtraceEvents, DropsNothing) {
  auto sourcePacket = protos::gen::TracePacket();

  auto* ftrace_events = sourcePacket.mutable_ftrace_events();
  ftrace_events->set_cpu(0);

  {
    auto* event = ftrace_events->add_event();
    event->set_timestamp(kTimes[0]);
    event->set_pid(kPid);

    auto* print = event->mutable_print();
    print->set_buf("TEXT A");
  }

  {
    auto* event = ftrace_events->add_event();
    event->set_timestamp(kTimes[1]);
    event->set_pid(kPid);

    auto* print = event->mutable_print();
    print->set_buf("TEXT B");
  }

  ASSERT_EQ(ftrace_events->event_size(), 2);

  Context context;
  auto inputBuffer = sourcePacket.SerializeAsString();

  DropEmptyFtraceEvents transform;
  ASSERT_TRUE(transform.Transform(context, &inputBuffer).ok());

  auto packet = protos::gen::TracePacket();
  packet.ParseFromString(inputBuffer);

  ASSERT_EQ(packet.ftrace_events().event_size(), 2);
}

// The first event is not empty (it has a print event). However, the second
// event does not have a body, and should be removed.
//
//  packet: {
//    ftrace_events: {
//      cpu  : 0x00000003
//      event: {
//        timestamp : 0x0000001d5d0ce35d
//        pid       : 0x00400005
//        print     : {
//          buf: "TEXT A"
//        }
//      }
//      event: {
//        timestamp : 0x0000001d5d0d7314
//        pid       : 0x00400005
//      }
//    }
//  }
TEST(DropEmptyFtraceEvents, DropsEvent) {
  auto sourcePacket = protos::gen::TracePacket();

  auto* ftrace_events = sourcePacket.mutable_ftrace_events();
  ftrace_events->set_cpu(0);

  {
    auto* event = ftrace_events->add_event();
    event->set_timestamp(kTimes[0]);
    event->set_pid(kPid);

    auto* print = event->mutable_print();
    print->set_buf("TEXT A");
  }

  {
    auto* event = ftrace_events->add_event();
    event->set_timestamp(kTimes[1]);
    event->set_pid(kPid);
  }

  ASSERT_EQ(ftrace_events->event_size(), 2);

  Context context;
  auto inputBuffer = sourcePacket.SerializeAsString();

  DropEmptyFtraceEvents transform;
  ASSERT_TRUE(transform.Transform(context, &inputBuffer).ok());

  auto packet = protos::gen::TracePacket();
  packet.ParseFromString(inputBuffer);

  ASSERT_EQ(packet.ftrace_events().event_size(), 1);
  ASSERT_TRUE(packet.ftrace_events().event()[0].has_print());
}

// Because all events have no bodies (only timestamp and pid), not only should
// they should they be removed, the whole ftrace_events should be removed.
//
//  packet: {
//    ftrace_events: {
//      cpu  : 0x00000003
//      event: {
//        timestamp     : 0x0000001d5d0ce35d
//        pid  : 0x00400005
//      }
//      event: {
//        timestamp     : 0x0000001d5d0d7314
//        pid  : 0x00400005
//      }
//    }
//  }
TEST(DropEmptyFtraceEvents, DropsFtraceEvents) {
  auto sourcePacket = protos::gen::TracePacket();

  auto* ftrace_events = sourcePacket.mutable_ftrace_events();
  ftrace_events->set_cpu(0);

  {
    auto* event = ftrace_events->add_event();
    event->set_timestamp(kTimes[0]);
    event->set_pid(kPid);
  }

  {
    auto* event = ftrace_events->add_event();
    event->set_timestamp(kTimes[1]);
    event->set_pid(kPid);
  }

  ASSERT_EQ(ftrace_events->event_size(), 2);

  Context context;
  auto inputBuffer = sourcePacket.SerializeAsString();

  DropEmptyFtraceEvents transform;
  ASSERT_TRUE(transform.Transform(context, &inputBuffer).ok());

  auto packet = protos::gen::TracePacket();
  packet.ParseFromString(inputBuffer);

  ASSERT_EQ(packet.ftrace_events().event_size(), 0);
}

}  // namespace perfetto::trace_redaction
