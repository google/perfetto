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

#include "test/gtest_and_gmock.h"

#include "src/android_sdk/perfetto_sdk_for_jni/tracing_sdk.h"
#include "src/shared_lib/test/utils.h"

#include "protos/perfetto/trace/interned_data/interned_data.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/track_event/track_event.gen.h"

namespace perfetto {
namespace {
using namespace perfetto::shlib::test_utils;

using protos::gen::Trace;
using protos::gen::TracePacket;
using protos::gen::TrackEvent;

sdk_for_jni::Session StartTracing() {
  std::vector<uint8_t> build_proto_config =
      TracingSession::Builder()
          .set_data_source_name("track_event")
          .add_enabled_category("*")  // enable everything
          .BuildProtoConfig();
  return sdk_for_jni::Session(true, build_proto_config.data(),
                              build_proto_config.size());
}

Trace StopTracing(sdk_for_jni::Session& tracing_session) {
  tracing_session.FlushBlocking(5000);
  tracing_session.StopBlocking();
  std::vector trace_data(tracing_session.ReadBlocking());
  Trace trace;
  trace.ParseFromArray(trace_data.data(), trace_data.size());
  return trace;
}

// Smoke test for the Low Level emit path. The body and frame the JNI layer
// assembles are exercised end-to-end by the Java host tests; here we just check
// emit_track_event drives the LL ABI and produces the expected track events.
TEST(TracingSdkForJniTest, EmitsSlice) {
  sdk_for_jni::register_perfetto(true);
  sdk_for_jni::Category category("rendering");
  category.register_category();

  auto tracing_session = StartTracing();

  sdk_for_jni::emit_track_event(
      category.get(), PERFETTO_TE_TYPE_SLICE_BEGIN, "DrawPlayer",
      /*body=*/nullptr, /*body_size=*/0, /*set_track_uuid=*/false,
      /*leaf_track_uuid=*/0, /*track_count=*/0, nullptr, nullptr, nullptr,
      /*track_name_static=*/false, /*track_is_counter=*/false,
      /*interned_count=*/0, nullptr, nullptr, nullptr);
  sdk_for_jni::emit_track_event(
      category.get(), PERFETTO_TE_TYPE_SLICE_END, "DrawPlayer",
      /*body=*/nullptr, /*body_size=*/0, /*set_track_uuid=*/false,
      /*leaf_track_uuid=*/0, /*track_count=*/0, nullptr, nullptr, nullptr,
      /*track_name_static=*/false, /*track_is_counter=*/false,
      /*interned_count=*/0, nullptr, nullptr, nullptr);

  Trace trace = StopTracing(tracing_session);

  std::vector<int> event_types;
  std::vector<std::string> category_names;
  std::vector<std::string> event_names;
  for (const TracePacket& packet : trace.packet()) {
    if (packet.has_interned_data()) {
      for (const auto& cat : packet.interned_data().event_categories()) {
        category_names.push_back(cat.name());
      }
      for (const auto& name : packet.interned_data().event_names()) {
        event_names.push_back(name.name());
      }
    }
    if (packet.has_track_event()) {
      event_types.push_back(packet.track_event().type());
    }
  }

  EXPECT_THAT(event_types, testing::ElementsAre(TrackEvent::TYPE_SLICE_BEGIN,
                                                TrackEvent::TYPE_SLICE_END));
  EXPECT_THAT(category_names, testing::Contains("rendering"));
  EXPECT_THAT(event_names, testing::Contains("DrawPlayer"));
}
}  // namespace
}  // namespace perfetto
