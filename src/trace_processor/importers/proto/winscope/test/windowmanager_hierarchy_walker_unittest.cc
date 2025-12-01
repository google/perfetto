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

#include <string_view>

#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/server/windowmanagerservice.pbzero.h"
#include "src/trace_processor/importers/proto/winscope/test/windowmanager_sample_protos.h"
#include "src/trace_processor/importers/proto/winscope/windowmanager_hierarchy_walker.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope {

class WindowManagerHierarchyWalkerTest : public ::testing::Test {
 protected:
  struct ExpectedWindowContainer {
    std::string_view title;
    int32_t token;
    std::optional<int32_t> parent_token;
    std::optional<int32_t> child_index;
    bool is_visible;
    std::optional<WindowManagerHierarchyWalker::ExtractedRect> rect;
    std::string_view container_type;
    std::optional<std::string> name_override;
  };

  void CheckWindowContainers(
      const base::StatusOr<std::vector<
          WindowManagerHierarchyWalker::ExtractedWindowContainer>>& actual,
      const std::vector<ExpectedWindowContainer>& expected) const {
    EXPECT_TRUE(actual.ok());
    EXPECT_EQ(actual->size(), expected.size());

    for (size_t i = 0; i < actual->size(); ++i) {
      EXPECT_EQ(pool_.Get(actual->at(i).title).ToStdString(),
                expected[i].title);
      EXPECT_EQ(actual->at(i).token, expected[i].token);
      EXPECT_EQ(actual->at(i).parent_token, expected[i].parent_token);
      EXPECT_EQ(actual->at(i).child_index, expected[i].child_index);
      EXPECT_EQ(actual->at(i).is_visible, expected[i].is_visible);
      CheckRects(actual->at(i).rect, expected[i].rect);
      EXPECT_EQ(pool_.Get(actual->at(i).container_type).ToStdString(),
                expected[i].container_type);

      auto name_override_id = actual->at(i).name_override;
      std::optional<std::string> name_override;
      if (name_override_id.has_value()) {
        name_override = pool_.Get(name_override_id.value()).ToStdString();
      }
      EXPECT_EQ(name_override, expected[i].name_override);

      bool is_root = !expected[i].parent_token.has_value();
      if (is_root) {
        CheckRootWindowContainerProtoIsPruned(actual->at(i).pruned_proto);
      } else {
        CheckWindowContainerChildProtoIsPruned(actual->at(i).pruned_proto);
      }
    }
  }

  void CheckRects(
      const std::optional<WindowManagerHierarchyWalker::ExtractedRect>& actual,
      const std::optional<WindowManagerHierarchyWalker::ExtractedRect>&
          expected) const {
    EXPECT_EQ(actual.has_value(), expected.has_value());

    if (!expected.has_value()) {
      return;
    }

    EXPECT_EQ(actual->x, expected->x);
    EXPECT_EQ(actual->y, expected->y);
    EXPECT_EQ(actual->w, expected->w);
    EXPECT_EQ(actual->h, expected->h);
    EXPECT_EQ(actual->display_id, expected->display_id);
    EXPECT_EQ(actual->depth, expected->depth);
    EXPECT_EQ(actual->is_visible, expected->is_visible);
    EXPECT_EQ(actual->opacity, expected->opacity);
  }

  void CheckRootWindowContainerProtoIsPruned(
      const std::vector<uint8_t>& bytes) const {
    protos::pbzero::RootWindowContainerProto::Decoder root(bytes.data(),
                                                           bytes.size());
    EXPECT_TRUE(root.has_window_container());
    protos::pbzero::WindowContainerProto::Decoder window_container(
        root.window_container());
    EXPECT_FALSE(window_container.has_children());
  }

  void CheckWindowContainerChildProtoIsPruned(
      const std::vector<uint8_t>& bytes) const {
    protos::pbzero::WindowContainerChildProto::Decoder child(
        protozero::ConstBytes{bytes.data(), bytes.size()});

    if (child.has_window_container()) {
      protos::pbzero::WindowContainerProto::Decoder window_container(
          child.window_container());
      EXPECT_FALSE(window_container.has_children());
    } else if (child.has_display_content()) {
      protos::pbzero::DisplayContentProto::Decoder display_content(
          child.display_content());
      protos::pbzero::DisplayAreaProto::Decoder display_area(
          display_content.root_display_area());
      protos::pbzero::WindowContainerProto::Decoder window_container(
          display_area.window_container());
      EXPECT_FALSE(window_container.has_children());
    } else if (child.has_display_area()) {
      protos::pbzero::DisplayAreaProto::Decoder display_area(
          child.display_area());
      protos::pbzero::WindowContainerProto::Decoder window_container(
          display_area.window_container());
      EXPECT_FALSE(window_container.has_children());
    } else if (child.has_task()) {
      protos::pbzero::TaskProto::Decoder task(child.task());
      protos::pbzero::WindowContainerProto::Decoder deprecated_window_container(
          task.window_container());
      EXPECT_FALSE(deprecated_window_container.has_children());
      protos::pbzero::TaskFragmentProto::Decoder task_fragment(
          task.task_fragment());
      protos::pbzero::WindowContainerProto::Decoder window_container(
          task_fragment.window_container());
      EXPECT_FALSE(window_container.has_children());
    } else if (child.has_activity()) {
      protos::pbzero::ActivityRecordProto::Decoder activity(child.activity());
      protos::pbzero::WindowTokenProto::Decoder token(activity.window_token());
      protos::pbzero::WindowContainerProto::Decoder window_container(
          token.window_container());
      EXPECT_FALSE(window_container.has_children());
    } else if (child.has_window_token()) {
      protos::pbzero::WindowTokenProto::Decoder token(child.window_token());
      protos::pbzero::WindowContainerProto::Decoder window_container(
          token.window_container());
      EXPECT_FALSE(window_container.has_children());
    } else if (child.has_window()) {
      protos::pbzero::WindowStateProto::Decoder window_state(child.window());
      protos::pbzero::WindowContainerProto::Decoder window_container(
          window_state.window_container());
      EXPECT_FALSE(window_container.has_children());
    } else if (child.has_task_fragment()) {
      protos::pbzero::TaskFragmentProto::Decoder task_fragment(
          child.task_fragment());
      protos::pbzero::WindowContainerProto::Decoder window_container(
          task_fragment.window_container());
      EXPECT_FALSE(window_container.has_children());
    } else {
      FAIL();
    }
  }

  StringPool pool_;
  WindowManagerHierarchyWalker walker_{&pool_};
};

TEST_F(WindowManagerHierarchyWalkerTest, EmptyHierarchy) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::EmptyHierarchy()));
  EXPECT_TRUE(result.has_parse_error);
}

// Hierarchy:
// RootWindowContainerProto
TEST_F(WindowManagerHierarchyWalkerTest, HierarchyWithRootOnly) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithRootOnly()));
  EXPECT_FALSE(result.has_parse_error);
  CheckWindowContainers(result.window_containers,
                        {
                            {"root", 1, std::nullopt, std::nullopt, false,
                             std::nullopt, "RootWindowContainer", std::nullopt},
                        });
}

// Hierarchy:
// RootWindowContainerProto -> WindowContainerProto -> WindowContainerProto
TEST_F(WindowManagerHierarchyWalkerTest, HierarchyWithWindowContainerProto) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithWindowContainer()));
  EXPECT_FALSE(result.has_parse_error);
  CheckWindowContainers(result.window_containers,
                        {
                            {"root", 1, std::nullopt, std::nullopt, false,
                             std::nullopt, "RootWindowContainer", std::nullopt},
                            {"child - WindowContainer", 2, 1, 0, false,
                             std::nullopt, "WindowContainer", std::nullopt},
                            {"grandchild - WindowContainer", 3, 2, 0, false,
                             std::nullopt, "WindowContainer", std::nullopt},
                        });
}

// Hierarchy:
// RootWindowContainerProto -> DisplayContentProto -> WindowStateProto ->
// WindowContainerProto
TEST_F(WindowManagerHierarchyWalkerTest,
       HierarchyWithDisplayContentProtoAndWindowStateProto) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::
              HierarchyWithDisplayContentAndWindowState()));
  EXPECT_FALSE(result.has_parse_error);

  WindowManagerHierarchyWalker::ExtractedRect expectedRectDisplayContent{
      0, 0, 10, 20, 1, 0, false, std::nullopt};

  WindowManagerHierarchyWalker::ExtractedRect expectedRectWindowState{
      5, 6, 10, 20, 1, 1, true, 0.5};

  CheckWindowContainers(
      result.window_containers,
      {
          {"root", 1, std::nullopt, std::nullopt, false, std::nullopt,
           "RootWindowContainer", std::nullopt},
          {"child - DisplayContent", 2, 1, 0, false, expectedRectDisplayContent,
           "DisplayContent", std::nullopt},
          {"grandchild - WindowState", 3, 2, 0, true, expectedRectWindowState,
           "WindowState", std::nullopt},
          {"grandgrandchild - WindowContainer", 4, 3, 0, false, std::nullopt,
           "WindowContainer", std::nullopt},
      });
}

// Hierarchy:
// RootWindowContainerProto -> DisplayAreaProto -> WindowContainerProto
TEST_F(WindowManagerHierarchyWalkerTest, HierarchyWithDisplayAreaProto) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithDisplayArea()));
  EXPECT_FALSE(result.has_parse_error);

  CheckWindowContainers(result.window_containers,
                        {
                            {"root", 1, std::nullopt, std::nullopt, false,
                             std::nullopt, "RootWindowContainer", std::nullopt},
                            {"child - DisplayArea", 2, 1, 0, false,
                             std::nullopt, "DisplayArea", std::nullopt},
                            {"grandchild - WindowContainer", 3, 2, 0, false,
                             std::nullopt, "WindowContainer", std::nullopt},
                        });
}

// Hierarchy:
// RootWindowContainerProto -> TaskProto -> WindowContainerProto
TEST_F(WindowManagerHierarchyWalkerTest, HierarchyWithTaskProto) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithTask()));
  EXPECT_FALSE(result.has_parse_error);

  CheckWindowContainers(
      result.window_containers,
      {
          {"root", 1, std::nullopt, std::nullopt, false, std::nullopt,
           "RootWindowContainer", std::nullopt},
          {"child - Task", 2, 1, 0, false, std::nullopt, "Task", std::nullopt},
          {"grandchild - WindowContainer", 3, 2, 0, false, std::nullopt,
           "WindowContainer", std::nullopt},
      });
}

// Hierarchy:
// RootWindowContainerProto -> ActivityRecordProto -> WindowContainerProto
TEST_F(WindowManagerHierarchyWalkerTest, HierarchyWithActivityRecordProto) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithActivityRecord()));
  EXPECT_FALSE(result.has_parse_error);

  CheckWindowContainers(result.window_containers,
                        {
                            {"root", 1, std::nullopt, std::nullopt, false,
                             std::nullopt, "RootWindowContainer", std::nullopt},
                            {"child - ActivityRecord", 2, 1, 0, false,
                             std::nullopt, "Activity", std::nullopt},
                            {"grandchild - WindowContainer", 3, 2, 0, false,
                             std::nullopt, "WindowContainer", std::nullopt},
                        });
}

// Hierarchy:
// RootWindowContainerProto -> WindowTokenProto -> WindowContainerProto
TEST_F(WindowManagerHierarchyWalkerTest, HierarchyWithWindowTokenProto) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithWindowToken()));
  EXPECT_FALSE(result.has_parse_error);

  CheckWindowContainers(
      result.window_containers,
      {
          {"root", 1, std::nullopt, std::nullopt, false, std::nullopt,
           "RootWindowContainer", std::nullopt},
          {"0x02", 2, 1, 0, false, std::nullopt, "WindowToken", std::nullopt},
          {"grandchild - WindowContainer", 3, 2, 0, false, std::nullopt,
           "WindowContainer", std::nullopt},
      });
}

// Hierarchy:
// RootWindowContainerProto -> TaskFragmentProto -> WindowContainerProto
TEST_F(WindowManagerHierarchyWalkerTest, HierarchyWithTaskFragmentProto) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithTaskFragment()));
  EXPECT_FALSE(result.has_parse_error);

  CheckWindowContainers(result.window_containers,
                        {
                            {"root", 1, std::nullopt, std::nullopt, false,
                             std::nullopt, "RootWindowContainer", std::nullopt},
                            {"child - TaskFragment", 2, 1, 0, false,
                             std::nullopt, "TaskFragment", std::nullopt},
                            {"grandchild - WindowContainer", 3, 2, 0, false,
                             std::nullopt, "WindowContainer", std::nullopt},
                        });
}

// Hierarchy:
//
//           RootWindowContainerProto
//               │              │
//               │              │
//               ▼              ▼
// WindowContainerProto     WindowContainerProto
TEST_F(WindowManagerHierarchyWalkerTest, HierarchyWithSiblings) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithSiblings()));
  EXPECT_FALSE(result.has_parse_error);

  CheckWindowContainers(result.window_containers,
                        {
                            {"root", 1, std::nullopt, std::nullopt, false,
                             std::nullopt, "RootWindowContainer", std::nullopt},
                            {"child - WindowContainer1", 2, 1, 0, false,
                             std::nullopt, "WindowContainer", std::nullopt},
                            {"child - WindowContainer2", 3, 1, 1, false,
                             std::nullopt, "WindowContainer", std::nullopt},
                        });
}

TEST_F(WindowManagerHierarchyWalkerTest, InvalidWindowContainerChildProto) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::InvalidWindowContainerChildProto()));
  EXPECT_TRUE(result.has_parse_error);

  CheckWindowContainers(result.window_containers,
                        {
                            {"root", 1, std::nullopt, std::nullopt, false,
                             std::nullopt, "RootWindowContainer", std::nullopt},
                        });
}

TEST_F(WindowManagerHierarchyWalkerTest, TaskNameOverride) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithTaskIdAndName()));
  EXPECT_FALSE(result.has_parse_error);

  CheckWindowContainers(
      result.window_containers,
      {
          {"root", 1, std::nullopt, std::nullopt, false, std::nullopt,
           "RootWindowContainer", std::nullopt},
          {"child - Task", 2, 1, 0, false, std::nullopt, "Task", "3(MockTask)"},
      });
}

TEST_F(WindowManagerHierarchyWalkerTest, TaskWindowContainerFallback) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithTaskContainerFallback()));
  EXPECT_FALSE(result.has_parse_error);

  CheckWindowContainers(
      result.window_containers,
      {
          {"root", 1, std::nullopt, std::nullopt, false, std::nullopt,
           "RootWindowContainer", std::nullopt},
          {"child - Task", 2, 1, 0, false, std::nullopt, "Task", std::nullopt},
          {"grandchild - WindowContainer", 3, 2, 0, false, std::nullopt,
           "WindowContainer", std::nullopt},
      });
}

TEST_F(WindowManagerHierarchyWalkerTest, WindowStateNameOverrides) {
  auto result = walker_.ExtractWindowContainers(
      protos::pbzero::WindowManagerTraceEntry::Decoder(
          WindowManagerSampleProtos::HierarchyWithWindowStateNameOverrides()));
  EXPECT_FALSE(result.has_parse_error);

  WindowManagerHierarchyWalker::ExtractedRect expectedRectWindowState1{
      0, 0, 0, 0, -1, 0, false, 0};
  WindowManagerHierarchyWalker::ExtractedRect expectedRectWindowState2{
      0, 0, 0, 0, -1, 1, false, 0};

  CheckWindowContainers(
      result.window_containers,
      {
          {"root", 1, std::nullopt, std::nullopt, false, std::nullopt,
           "RootWindowContainer", std::nullopt},
          {"Starting state - WindowState", 2, 1, 0, false,
           expectedRectWindowState1, "WindowState", "state - WindowState"},
          {"Waiting For Debugger: state - WindowState", 3, 1, 1, false,
           expectedRectWindowState2, "WindowState", "state - WindowState"},
      });
}
}  // namespace perfetto::trace_processor::winscope
