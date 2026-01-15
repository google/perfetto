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

#include "src/trace_processor/importers/proto/v8_tracker.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/importers/common/address_range.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::Eq;

class V8TrackerTest : public testing::Test {
 public:
  V8TrackerTest() : v8_tracker_(&context_) {
    context_.storage.reset(new TraceStorage());
    context_.process_tracker.reset(new ProcessTracker(&context_));
    context_.stack_profile_tracker.reset(new StackProfileTracker(&context_));
    context_.mapping_tracker.reset(new MappingTracker(&context_));
  }

 protected:
  TraceProcessorContext context_;
  V8Tracker v8_tracker_;
};

TEST_F(V8TrackerTest, AddICEvent) {
  const UniquePid upid = context_.process_tracker->GetOrCreateProcess(1234);
  const UniqueTid utid = context_.process_tracker->UpdateThread(4321, upid);
  const int64_t timestamp_early = 100;
  const int64_t timestamp = 1000;
  const uint64_t code_start_early = 0x0100;
  const uint64_t code_start = 0x1000;
  const uint64_t code_size = 10;
  const uint64_t ic_pc = code_start + 1;

  // 1. Create Isolate
  protozero::HeapBuffered<protos::pbzero::InternedV8Isolate> isolate_msg;
  isolate_msg->set_pid(upid);
  isolate_msg->set_isolate_id(1);
  isolate_msg->set_code_range()->set_base_address(code_start_early);
  isolate_msg->set_code_range()->set_size(code_start + code_size);
  std::vector<uint8_t> isolate_vec = isolate_msg.SerializeAsArray();
  auto isolate_id = v8_tracker_.InternIsolate(
      protozero::ConstBytes{isolate_vec.data(), isolate_vec.size()});
  ASSERT_TRUE(isolate_id.has_value());

  // 2. Create Script
  protozero::HeapBuffered<protos::pbzero::InternedV8JsScript> script_msg;
  script_msg->set_script_id(1);
  std::vector<uint8_t> script_vec = script_msg.SerializeAsArray();
  auto script_id = v8_tracker_.InternJsScript(
      protozero::ConstBytes{script_vec.data(), script_vec.size()}, *isolate_id);

  // 3. Create Function
  protozero::HeapBuffered<protos::pbzero::InternedV8JsFunction> func_msg;
  func_msg->set_v8_js_script_iid(1);
  std::vector<uint8_t> func_vec = func_msg.SerializeAsArray();
  auto func_id = v8_tracker_.InternJsFunction(
      protozero::ConstBytes{func_vec.data(), func_vec.size()},
      context_.storage->InternString("test_fn"), script_id);

  // 4. Add JS Code (which populates jit_tracker and jit_to_v8_js_code_)
  {
    protozero::HeapBuffered<protos::pbzero::V8JsCode> code_msg;
    code_msg->set_instruction_start(code_start_early);
    code_msg->set_instruction_size_bytes(code_size);
    code_msg->set_tier(protos::pbzero::V8JsCode::TIER_MAGLEV);
    code_msg->set_machine_code("ML_6543210");

    std::vector<uint8_t> decoder_vec = code_msg.SerializeAsArray();
    protos::pbzero::V8JsCode::Decoder code_decoder(decoder_vec.data(),
                                                   decoder_vec.size());

    v8_tracker_.AddJsCode(timestamp_early, utid, *isolate_id, func_id,
                          code_decoder);
  }
  {
    protozero::HeapBuffered<protos::pbzero::V8JsCode> code_msg;
    code_msg->set_instruction_start(code_start);
    code_msg->set_instruction_size_bytes(code_size);
    code_msg->set_tier(protos::pbzero::V8JsCode::TIER_TURBOFAN);
    code_msg->set_machine_code("TF_6543210");

    std::vector<uint8_t> decoder_vec = code_msg.SerializeAsArray();
    protos::pbzero::V8JsCode::Decoder code_decoder(decoder_vec.data(),
                                                   decoder_vec.size());
    v8_tracker_.AddJsCode(timestamp, utid, *isolate_id, func_id, code_decoder);
  }

  // Verify that the code was added
  ASSERT_EQ(context_.storage->v8_js_code_table().row_count(), 2u);

  // 5. Add IC Event
  protozero::HeapBuffered<protos::pbzero::V8ICEvent> ic_msg;
  ic_msg->set_pc(ic_pc);
  ic_msg->set_type("LoadIC");
  ic_msg->set_map(0x1234);

  std::vector<uint8_t> ic_vec = ic_msg.SerializeAsArray();
  protos::pbzero::V8ICEvent::Decoder ic_decoder(ic_vec.data(), ic_vec.size());
  v8_tracker_.AddICEvent(timestamp + 10, utid, *isolate_id, ic_decoder);

  // 6. Verify IC Event
  ASSERT_EQ(context_.storage->v8_ic_event_table().row_count(), 1u);
  auto ic_event = context_.storage->v8_ic_event_table()[0];
  EXPECT_EQ(ic_event.ts(), timestamp + 10);
  EXPECT_EQ(ic_event.utid(), utid);
  EXPECT_EQ(ic_event.v8_isolate_id(), *isolate_id);
  EXPECT_EQ(ic_event.pc(), static_cast<int64_t>(ic_pc));
  EXPECT_EQ(ic_event.map(), 0x1234);
  // Verify ic_event's linked to the correct code
  EXPECT_EQ(ic_event.v8_js_code_id().value, 1u);
}

}  // namespace
}  // namespace perfetto::trace_processor
