/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_TEST_WINDOWMANAGER_SAMPLE_PROTOS_V2_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_TEST_WINDOWMANAGER_SAMPLE_PROTOS_V2_H_

#include <cstdint>
#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/android/server/windowmanagerservice.pbzero.h"
#include "protos/perfetto/trace/android/windowmanager.pbzero.h"

namespace perfetto::trace_processor::winscope {

// Provides windowmanager sample protos with format v2
// (flat list of WindowContainerChildProto messages)
class WindowManagerSampleProtosV2 {
 public:
  static std::string EmptyHierarchy() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    entry->set_window_manager_service();
    return entry.SerializeAsString();
  }

  static std::string HierarchyWithRootOnly() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    auto* wrapper = service->add_window_containers();
    wrapper->set_token(1);
    auto* window_container = wrapper->set_window_container();
    auto* identifier = window_container->set_identifier();
    identifier->set_hash_code(1);
    identifier->set_title("root");

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithWindowContainer() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
    }

    // Child - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(2);
      id->set_title("child - WindowContainer");
      window_container->add_child_tokens(3);
    }

    // Grandchild - WindowContainerProto
    auto* wrapper = service->add_window_containers();
    wrapper->set_token(3);
    auto* window_container = wrapper->set_window_container();
    auto* id = window_container->set_identifier();
    id->set_hash_code(3);
    id->set_title("grandchild - WindowContainer");

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithDisplayContentProtoAndWindowStateProto() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
    }

    // Child - DisplayContentProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* display_content = wrapper->set_display_content();
      display_content->set_id(1);

      auto* display_info = display_content->set_display_info();
      display_info->set_name("child - DisplayContent");
      display_info->set_logical_width(10);
      display_info->set_logical_height(20);

      auto* wc =
          display_content->set_root_display_area()->set_window_container();
      wc->set_identifier()->set_hash_code(2);
      wc->add_child_tokens(3);
    }

    // Grandchild - WindowStateProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(3);
      auto* window_state = wrapper->set_window();
      window_state->set_is_visible(true);

      auto* attributes = window_state->set_attributes();
      attributes->set_alpha(0.5);

      auto* frame = window_state->set_window_frames()->set_frame();
      frame->set_left(5);
      frame->set_top(6);
      frame->set_right(15);
      frame->set_bottom(26);

      auto* wc = window_state->set_window_container();
      auto* id = wc->set_identifier();
      id->set_hash_code(3);
      id->set_title("grandchild - WindowState");
      wc->add_child_tokens(4);
    }

    // Grandgrandchild - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(4);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(4);
      id->set_title("grandgrandchild - WindowContainer");
    }

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithDisplayArea() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
    }

    // Child - DisplayAreaProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* display_area = wrapper->set_display_area();
      display_area->set_name("child - DisplayArea");

      auto* wc = display_area->set_window_container();
      wc->set_identifier()->set_hash_code(2);
      wc->add_child_tokens(3);
    }

    // Grandchild - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(3);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(3);
      id->set_title("grandchild - WindowContainer");
    }

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithTask() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
    }

    // Child - TaskProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* task = wrapper->set_task();
      auto* task_fragment = task->set_task_fragment();
      auto* window_container = task_fragment->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(2);
      id->set_title("child - Task");
      window_container->add_child_tokens(3);
    }

    // Grandchild - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(3);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(3);
      id->set_title("grandchild - WindowContainer");
    }

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithActivityRecord() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
    }

    // Child - ActivityRecordProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* activity = wrapper->set_activity();
      activity->set_name("child - ActivityRecord");
      auto* window_token = activity->set_window_token();
      window_token->set_hash_code(2);
      auto* window_container = window_token->set_window_container();
      window_container->add_child_tokens(3);
    }

    // Grandchild - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(3);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(3);
      id->set_title("grandchild - WindowContainer");
    }

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithWindowToken() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
    }

    // Child - WindowTokenProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* window_token = wrapper->set_window_token();
      window_token->set_hash_code(2);
      auto* window_container = window_token->set_window_container();
      window_container->add_child_tokens(3);
    }

    // Grandchild - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(3);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(3);
      id->set_title("grandchild - WindowContainer");
    }

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithTaskFragment() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
    }

    // Child - TaskFragmentProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* task_fragment = wrapper->set_task_fragment();
      auto* window_container = task_fragment->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(2);
      id->set_title("child - TaskFragment");
      window_container->add_child_tokens(3);
    }

    // Grandchild - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(3);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(3);
      id->set_title("grandchild - WindowContainer");
    }

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithSiblings() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
      window_container->add_child_tokens(3);
    }

    // Child 1 - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(2);
      id->set_title("child - WindowContainer1");
    }

    // Child 2 - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(3);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(3);
      id->set_title("child - WindowContainer2");
    }

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithTaskIdAndName() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
    }

    // Child - TaskProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* task = wrapper->set_task();
      task->set_id(3);
      task->set_task_name("MockTask");
      auto* task_fragment = task->set_task_fragment();
      auto* window_container = task_fragment->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(2);
      id->set_title("child - Task");
    }

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithTaskContainerFallback() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
    }

    // Child - TaskProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* task = wrapper->set_task();
      auto* task_fragment = task->set_task_fragment();
      auto* window_container = task_fragment->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(2);
      id->set_title("child - Task");
      window_container->add_child_tokens(3);  // Link to grandchild!
    }

    // Grandchild - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(3);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(3);
      id->set_title("grandchild - WindowContainer");
    }

    return entry.SerializeAsString();
  }

  static std::string HierarchyWithWindowStateNameOverrides() {
    protozero::HeapBuffered<protos::pbzero::WindowManagerTraceEntry> entry;
    auto* service = entry->set_window_manager_service();
    service->set_root_window_container();

    // Root - WindowContainerProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(1);
      auto* window_container = wrapper->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(1);
      id->set_title("root");
      window_container->add_child_tokens(2);
      window_container->add_child_tokens(3);
    }

    // Child 1 - WindowStateProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(2);
      auto* window_state = wrapper->set_window();
      auto* window_container = window_state->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(2);
      id->set_title("Starting state - WindowState");
    }

    // Child 2 - WindowStateProto
    {
      auto* wrapper = service->add_window_containers();
      wrapper->set_token(3);
      auto* window_state = wrapper->set_window();
      auto* window_container = window_state->set_window_container();
      auto* id = window_container->set_identifier();
      id->set_hash_code(3);
      id->set_title("Waiting For Debugger: state - WindowState");
    }

    return entry.SerializeAsString();
  }
};

}  // namespace perfetto::trace_processor::winscope

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_TEST_WINDOWMANAGER_SAMPLE_PROTOS_V2_H_
