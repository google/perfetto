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

#include "src/trace_processor/importers/proto/winscope/windowmanager_hierarchy_walker.h"

#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "protos/perfetto/trace/android/graphics/rect.pbzero.h"
#include "protos/perfetto/trace/android/view/displayinfo.pbzero.h"
#include "protos/perfetto/trace/android/view/windowlayoutparams.pbzero.h"
#include "protos/perfetto/trace/android/windowmanager.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/proto/winscope/windowmanager_proto_clone.h"

namespace perfetto::trace_processor::winscope {

WindowManagerHierarchyWalker::WindowManagerHierarchyWalker(StringPool* pool)
    : pool_(pool) {}

base::StatusOr<
    std::vector<WindowManagerHierarchyWalker::ExtractedWindowContainer>>
WindowManagerHierarchyWalker::ExtractWindowContainers(
    const protos::pbzero::WindowManagerTraceEntry::Decoder& entry) {
  protos::pbzero::WindowManagerServiceDumpProto::Decoder service(
      entry.window_manager_service());
  protos::pbzero::RootWindowContainerProto::Decoder root(
      service.root_window_container());

  std::vector<ExtractedWindowContainer> result;

  RETURN_IF_ERROR(ParseRootWindowContainer(root, &result));

  return base::StatusOr(std::move(result));
}

base::Status WindowManagerHierarchyWalker::ParseRootWindowContainer(
    const protos::pbzero::RootWindowContainerProto::Decoder& root,
    std::vector<ExtractedWindowContainer>* result) {
  if (!root.has_window_container()) {
    return base::ErrStatus(kErrorMessageMissingField);
  }

  protos::pbzero::WindowContainerProto::Decoder window_container(
      root.window_container());
  protos::pbzero::IdentifierProto::Decoder identifier(
      window_container.identifier());

  auto tokenAndTitle = ParseIdentifierProto(identifier);
  RETURN_IF_ERROR(tokenAndTitle.status());

  auto pruned_proto =
      windowmanager_proto_clone::CloneRootWindowContainerProtoPruningChildren(
          root);

  result->push_back(ExtractedWindowContainer{
      tokenAndTitle->title, tokenAndTitle->token, std::nullopt, std::nullopt,
      window_container.visible(), std::nullopt, std::move(pruned_proto)});

  return ParseWindowContainerChildren(window_container, tokenAndTitle->token,
                                      result);
}

base::Status WindowManagerHierarchyWalker::ParseWindowContainerChildren(
    const protos::pbzero::WindowContainerProto::Decoder& window_container,
    int32_t parent_token,
    std::vector<ExtractedWindowContainer>* result) {
  uint32_t index = 0;
  for (auto it = window_container.children(); it; ++it) {
    protos::pbzero::WindowContainerChildProto::Decoder child(*it);
    RETURN_IF_ERROR(
        ParseWindowContainerChildProto(child, parent_token, index, result));
    ++index;
  }
  return base::OkStatus();
}

base::Status WindowManagerHierarchyWalker::ParseWindowContainerChildProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t child_index,
    std::vector<ExtractedWindowContainer>* result) {
  if (child.has_window_container()) {
    return ParseWindowContainerProto(child, parent_token, child_index, result);
  }
  if (child.has_display_content()) {
    return ParseDisplayContentProto(child, parent_token, child_index, result);
  }
  if (child.has_display_area()) {
    return ParseDisplayAreaProto(child, parent_token, child_index, result);
  }
  if (child.has_task()) {
    return ParseTaskProto(child, parent_token, child_index, result);
  }
  if (child.has_activity()) {
    return ParseActivityRecordProto(child, parent_token, child_index, result);
  }
  if (child.has_window_token()) {
    return ParseWindowTokenProto(child, parent_token, child_index, result);
  }
  if (child.has_window()) {
    return ParseWindowStateProto(child, parent_token, child_index, result);
  }
  if (child.has_task_fragment()) {
    return ParseTaskFragmentProto(child, parent_token, child_index, result);
  }
  return base::ErrStatus(kErrorMessageMissingField);
}

base::Status WindowManagerHierarchyWalker::ParseWindowContainerProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t child_index,
    std::vector<ExtractedWindowContainer>* result) {
  protos::pbzero::WindowContainerProto::Decoder window_container(
      child.window_container());
  protos::pbzero::IdentifierProto::Decoder identifier(
      window_container.identifier());

  auto tokenAndTitle = ParseIdentifierProto(identifier);
  RETURN_IF_ERROR(tokenAndTitle.status());
  auto pruned_proto =
      windowmanager_proto_clone::CloneWindowContainerChildProtoPruningChildren(
          child);

  result->push_back(ExtractedWindowContainer{
      tokenAndTitle->title, tokenAndTitle->token, parent_token, child_index,
      window_container.visible(), std::nullopt, std::move(pruned_proto)});

  return ParseWindowContainerChildren(window_container, tokenAndTitle->token,
                                      result);
}

base::Status WindowManagerHierarchyWalker::ParseDisplayContentProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t child_index,
    std::vector<ExtractedWindowContainer>* result) {
  protos::pbzero::DisplayContentProto::Decoder display_content(
      child.display_content());
  protos::pbzero::DisplayAreaProto::Decoder display_area(
      display_content.root_display_area());
  protos::pbzero::DisplayInfoProto::Decoder display_info(
      display_content.display_info());
  protos::pbzero::WindowContainerProto::Decoder window_container(
      display_area.window_container());
  protos::pbzero::IdentifierProto::Decoder identifier(
      window_container.identifier());

  if (!identifier.has_hash_code()) {
    return base::ErrStatus(kErrorMessageMissingField);
  }
  if (!display_info.has_name()) {
    return base::ErrStatus(kErrorMessageMissingField);
  }

  int32_t token = identifier.hash_code();
  auto title = pool_->InternString(display_info.name());
  ExtractedRect display{-0,
                        0,
                        display_info.logical_width(),
                        display_info.logical_height(),
                        display_content.id(),
                        0,
                        false,
                        std::nullopt};
  auto pruned_proto =
      windowmanager_proto_clone::CloneWindowContainerChildProtoPruningChildren(
          child);

  result->push_back(ExtractedWindowContainer{
      title, token, parent_token, child_index, window_container.visible(),
      display, std::move(pruned_proto)});

  current_display_id_ = display_content.id();
  current_rect_depth_ = 1;

  return ParseWindowContainerChildren(window_container, token, result);
}

base::Status WindowManagerHierarchyWalker::ParseDisplayAreaProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t child_index,
    std::vector<ExtractedWindowContainer>* result) {
  protos::pbzero::DisplayAreaProto::Decoder display_area(child.display_area());
  protos::pbzero::WindowContainerProto::Decoder window_container(
      display_area.window_container());
  protos::pbzero::IdentifierProto::Decoder identifier(
      window_container.identifier());

  if (!identifier.has_hash_code()) {
    return base::ErrStatus(kErrorMessageMissingField);
  }
  if (!display_area.has_name()) {
    return base::ErrStatus(kErrorMessageMissingField);
  }

  auto token = identifier.hash_code();
  auto title = pool_->InternString(display_area.name());

  auto pruned_proto =
      windowmanager_proto_clone::CloneWindowContainerChildProtoPruningChildren(
          child);

  result->push_back(ExtractedWindowContainer{
      title, token, parent_token, child_index, window_container.visible(),
      std::nullopt, std::move(pruned_proto)});

  return ParseWindowContainerChildren(window_container, token, result);
}

base::Status WindowManagerHierarchyWalker::ParseTaskProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t child_index,
    std::vector<ExtractedWindowContainer>* result) {
  protos::pbzero::TaskProto::Decoder task(child.task());
  protos::pbzero::TaskFragmentProto::Decoder task_fragment(
      task.task_fragment());
  protos::pbzero::WindowContainerProto::Decoder window_container(
      task_fragment.window_container());
  protos::pbzero::IdentifierProto::Decoder identifier(
      window_container.identifier());

  auto tokenAndTitle = ParseIdentifierProto(identifier);
  RETURN_IF_ERROR(tokenAndTitle.status());
  auto pruned_proto =
      windowmanager_proto_clone::CloneWindowContainerChildProtoPruningChildren(
          child);

  result->push_back(ExtractedWindowContainer{
      tokenAndTitle->title, tokenAndTitle->token, parent_token, child_index,
      window_container.visible(), std::nullopt, std::move(pruned_proto)});

  return ParseWindowContainerChildren(window_container, tokenAndTitle->token,
                                      result);
}

base::Status WindowManagerHierarchyWalker::ParseActivityRecordProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t child_index,
    std::vector<ExtractedWindowContainer>* result) {
  protos::pbzero::ActivityRecordProto::Decoder activity(child.activity());
  protos::pbzero::WindowTokenProto::Decoder window_token(
      activity.window_token());
  protos::pbzero::WindowContainerProto::Decoder window_container(
      window_token.window_container());

  if (!window_token.has_hash_code()) {
    return base::ErrStatus(kErrorMessageMissingField);
  }
  if (!activity.has_name()) {
    return base::ErrStatus(kErrorMessageMissingField);
  }

  auto token = window_token.hash_code();
  auto title = pool_->InternString(activity.name());
  auto pruned_proto =
      windowmanager_proto_clone::CloneWindowContainerChildProtoPruningChildren(
          child);

  result->push_back(ExtractedWindowContainer{
      title, token, parent_token, child_index, activity.visible(), std::nullopt,
      std::move(pruned_proto)});

  return ParseWindowContainerChildren(window_container, token, result);
}

base::Status WindowManagerHierarchyWalker::ParseWindowTokenProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t child_index,
    std::vector<ExtractedWindowContainer>* result) {
  protos::pbzero::WindowTokenProto::Decoder window_token(child.window_token());
  protos::pbzero::WindowContainerProto::Decoder window_container(
      window_token.window_container());

  if (!window_token.has_hash_code()) {
    return base::ErrStatus(kErrorMessageMissingField);
  }

  auto token = window_token.hash_code();
  auto tokenHex = base::IntToHexString(static_cast<uint32_t>(token));
  auto title = pool_->InternString(base::StringView(tokenHex));
  auto pruned_proto =
      windowmanager_proto_clone::CloneWindowContainerChildProtoPruningChildren(
          child);

  result->push_back(ExtractedWindowContainer{
      title, token, parent_token, child_index, window_container.visible(),
      std::nullopt, std::move(pruned_proto)});

  return ParseWindowContainerChildren(window_container, token, result);
}

base::Status WindowManagerHierarchyWalker::ParseWindowStateProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t child_index,
    std::vector<ExtractedWindowContainer>* result) {
  protos::pbzero::WindowStateProto::Decoder window_state(child.window());
  protos::pbzero::WindowContainerProto::Decoder window_container(
      window_state.window_container());
  protos::pbzero::IdentifierProto::Decoder identifier(
      window_container.identifier());
  protos::pbzero::WindowLayoutParamsProto::Decoder attributes(
      window_state.attributes());
  protos::pbzero::WindowFramesProto::Decoder window_frames(
      window_state.window_frames());
  protos::pbzero::RectProto::Decoder frame(window_frames.frame());

  auto tokenAndTitle = ParseIdentifierProto(identifier);
  RETURN_IF_ERROR(tokenAndTitle.status());
  auto pruned_proto =
      windowmanager_proto_clone::CloneWindowContainerChildProtoPruningChildren(
          child);
  ExtractedRect rect{frame.left(),
                     frame.top(),
                     frame.right() - frame.left(),
                     frame.bottom() - frame.top(),
                     current_display_id_,
                     current_rect_depth_++,
                     window_state.is_visible(),
                     attributes.alpha()};

  result->push_back(ExtractedWindowContainer{
      tokenAndTitle->title, tokenAndTitle->token, parent_token, child_index,
      window_state.is_visible(), rect, std::move(pruned_proto)});

  return ParseWindowContainerChildren(window_container, tokenAndTitle->token,
                                      result);
}

base::Status WindowManagerHierarchyWalker::ParseTaskFragmentProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t child_index,
    std::vector<ExtractedWindowContainer>* result) {
  protos::pbzero::TaskFragmentProto::Decoder task_fragment(
      child.task_fragment());
  protos::pbzero::WindowContainerProto::Decoder window_container(
      task_fragment.window_container());
  protos::pbzero::IdentifierProto::Decoder identifier(
      window_container.identifier());

  auto tokenAndTitle = ParseIdentifierProto(identifier);
  RETURN_IF_ERROR(tokenAndTitle.status());
  auto pruned_proto =
      windowmanager_proto_clone::CloneWindowContainerChildProtoPruningChildren(
          child);

  result->push_back(ExtractedWindowContainer{
      tokenAndTitle->title, tokenAndTitle->token, parent_token, child_index,
      window_container.visible(), std::nullopt, std::move(pruned_proto)});

  return ParseWindowContainerChildren(window_container, tokenAndTitle->token,
                                      result);
}

base::StatusOr<WindowManagerHierarchyWalker::TokenAndTitle>
WindowManagerHierarchyWalker::ParseIdentifierProto(
    const protos::pbzero::IdentifierProto::Decoder& identifier) {
  if (!identifier.has_title() || !identifier.has_hash_code()) {
    return base::ErrStatus(kErrorMessageMissingField);
  }
  int32_t token = identifier.hash_code();
  auto title = pool_->InternString(identifier.title());
  return base::StatusOr(TokenAndTitle{token, title});
}

}  // namespace perfetto::trace_processor::winscope
