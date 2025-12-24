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

#include "perfetto/ext/base/string_utils.h"

#include "protos/perfetto/trace/interned_data/interned_data.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/track_event/debug_annotation.gen.h"
#include "protos/perfetto/trace/track_event/track_event.gen.h"

namespace perfetto {
namespace {
using namespace perfetto::shlib::test_utils;

using protos::gen::DebugAnnotation;
using protos::gen::EventCategory;
using protos::gen::EventName;
using protos::gen::InternedData;
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

template <class T>
std::vector<std::string> GetNames(const std::vector<T>& items) {
  std::vector<std::string> names;
  names.reserve(items.size());
  for (const auto& item : items) {
    names.push_back(item.name());
  }
  return names;
}

std::string DebugAnnotationToString(const DebugAnnotation& annotation) {
  std::stringstream ss;
  if (annotation.has_int_value()) {
    ss << "int: " << annotation.int_value();
  } else if (annotation.has_bool_value()) {
    ss << "bool: " << annotation.bool_value();
  } else {
    ss << "unexpected";
  }
  return ss.str();
}

std::string packet_to_string(const TracePacket& packet) {
  std::stringstream ss;
  ss << "packet {\n";
  if (packet.has_interned_data()) {
    ss << "data {";
    const InternedData& interned_data = packet.interned_data();
    ss << " categories: ["
       << base::Join(GetNames(interned_data.event_categories()), ", ") << "]";
    ss << " names: [" << base::Join(GetNames(interned_data.event_names()), ", ")
       << "],";
    ss << " debug_annotation_names: ["
       << base::Join(GetNames(interned_data.debug_annotation_names()), ", ")
       << "]";
    ss << " }\n";
  }
  if (packet.has_track_event()) {
    const TrackEvent& track_event = packet.track_event();
    ss << "event {";
    ss << " type: " << track_event.type() << ", ";
    std::vector<std::string> annotation_values;
    for (const auto& annotation : track_event.debug_annotations()) {
      annotation_values.push_back(DebugAnnotationToString(annotation));
    }
    ss << "debug_annotations: [" << base::Join(annotation_values, ", ") << "]";
    ss << " }\n";
  }
  ss << "}\n";
  return ss.str();
}

TEST(TracingSdkForJniTest, mySimpleTest) {
  sdk_for_jni::register_perfetto(true);
  sdk_for_jni::Category category("rendering");
  category.register_category();

  auto tracing_session = StartTracing();

  // In this test we generate a named slice with an additional payload

  sdk_for_jni::DebugArg player_number_extra("player_number");
  player_number_extra.get()->arg_int64.header.type =
      PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64;
  player_number_extra.get()->arg_int64.name = player_number_extra.name();
  player_number_extra.get()->arg_int64.value = 42;

  sdk_for_jni::DebugArg player_alive_extra("player_alive");
  player_alive_extra.get()->arg_bool.header.type =
      PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL;
  player_alive_extra.get()->arg_bool.name = player_alive_extra.name();
  player_alive_extra.get()->arg_bool.value = true;

  sdk_for_jni::Extra extra;
  extra.push_extra(reinterpret_cast<PerfettoTeHlExtra*>(
      &player_number_extra.get()->arg_int64));
  extra.push_extra(reinterpret_cast<PerfettoTeHlExtra*>(
      &player_alive_extra.get()->arg_bool));
  trace_event(PERFETTO_TE_TYPE_SLICE_BEGIN, category.get(), "DrawPlayer",
              &extra);

  sdk_for_jni::Extra empty_extra;
  trace_event(PERFETTO_TE_TYPE_SLICE_END, category.get(), "DrawPlayer",
              &empty_extra);

  Trace trace = StopTracing(tracing_session);

  std::string result;
  for (const TracePacket& packet : trace.packet()) {
    if (packet.has_interned_data() || packet.has_track_event()) {
      result += packet_to_string(packet);
    }
  }

  const char* actual = R"(packet {
data { categories: [rendering] names: [DrawPlayer], debug_annotation_names: [player_number, player_alive] }
event { type: 1, debug_annotations: [int: 42, bool: 1] }
}
packet {
event { type: 2, debug_annotations: [] }
}
)";

  EXPECT_STREQ(result.c_str(), actual);
}
}  // namespace
}  // namespace perfetto
