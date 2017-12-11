/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "event_info.h"

#include "gtest/gtest.h"

namespace perfetto {
namespace {

TEST(GetStaticEventInfo, SanityCheck) {
  std::vector<Event> events = GetStaticEventInfo();
  for (const Event& event : events) {
    // For each event the following fields should be filled
    // statically:
    // Non-empty name.
    ASSERT_TRUE(event.name);
    // Non-empty group.
    ASSERT_TRUE(event.group);
    // Non-zero proto field id.
    ASSERT_TRUE(event.proto_field_id);
    // Zero the ftrace id.
    ASSERT_FALSE(event.ftrace_event_id);

    for (const Field& field : event.fields) {
      // Non-empty name.
      ASSERT_TRUE(field.ftrace_name);
      // Non-zero proto field id.
      ASSERT_TRUE(field.proto_field_id);
      // Should have set the proto field type.
      ASSERT_TRUE(field.proto_field_type);
      // Other fields should be zeroed.
      ASSERT_FALSE(field.ftrace_offset);
      ASSERT_FALSE(field.ftrace_size);
      ASSERT_FALSE(field.ftrace_type);
    }
  }
}

}  // namespace
}  // namespace perfetto
