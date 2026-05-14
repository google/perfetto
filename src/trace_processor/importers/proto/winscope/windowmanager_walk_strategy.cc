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

#include "src/trace_processor/importers/proto/winscope/windowmanager_walk_strategy.h"
#include "protos/perfetto/trace/android/server/windowmanagerservice.pbzero.h"

namespace perfetto::trace_processor::winscope {

WalkStrategy::~WalkStrategy() = default;

DfsWalkStrategy::DfsWalkStrategy() = default;

DfsWalkStrategy::~DfsWalkStrategy() = default;

void DfsWalkStrategy::Walk(
    const protos::pbzero::WindowManagerTraceEntry::Decoder& entry,
    const std::function<
        void(const protos::pbzero::RootWindowContainerProto::Decoder&,
             const protos::pbzero::WindowContainerProto::Decoder&)>& onRoot,
    const std::function<
        void(const protos::pbzero::WindowContainerChildProto::Decoder&,
             int32_t parent_token,
             uint32_t child_index)>& onChild) {
  protos::pbzero::WindowManagerServiceDumpProto::Decoder service(
      entry.window_manager_service());
  protos::pbzero::RootWindowContainerProto::Decoder root(
      service.root_window_container());

  protos::pbzero::WindowContainerProto::Decoder window_container(
      root.window_container());

  onRoot(root, window_container);

  if (!root.has_window_container())
    return;

  protos::pbzero::IdentifierProto::Decoder identifier(
      window_container.identifier());
  int32_t root_token = identifier.hash_code();

  ParseWindowContainerChildren(window_container, root_token, onChild);
}

void DfsWalkStrategy::ParseWindowContainerChildren(
    const protos::pbzero::WindowContainerProto::Decoder& window_container,
    int32_t parent_token,
    const std::function<
        void(const protos::pbzero::WindowContainerChildProto::Decoder&,
             int32_t parent_token,
             uint32_t child_index)>& onChild) {
  uint32_t index = 0;
  for (auto it = window_container.children(); it; ++it) {
    protos::pbzero::WindowContainerChildProto::Decoder child(*it);
    ParseWindowContainerChildProto(child, parent_token, index, onChild);
    ++index;
  }
}

void DfsWalkStrategy::ParseWindowContainerChildProto(
    const protos::pbzero::WindowContainerChildProto::Decoder& child,
    int32_t parent_token,
    uint32_t index,
    const std::function<
        void(const protos::pbzero::WindowContainerChildProto::Decoder&,
             int32_t parent_token,
             uint32_t child_index)>& onChild) {
  onChild(child, parent_token, index);

  if (child.has_window_container()) {
    protos::pbzero::WindowContainerProto::Decoder window_container(
        child.window_container());
    protos::pbzero::IdentifierProto::Decoder identifier(
        window_container.identifier());
    ParseWindowContainerChildren(window_container, identifier.hash_code(),
                                 onChild);
  } else if (child.has_display_content()) {
    protos::pbzero::DisplayContentProto::Decoder display_content(
        child.display_content());
    protos::pbzero::DisplayAreaProto::Decoder display_area(
        display_content.root_display_area());
    protos::pbzero::WindowContainerProto::Decoder window_container(
        display_area.window_container());
    protos::pbzero::IdentifierProto::Decoder identifier(
        window_container.identifier());
    ParseWindowContainerChildren(window_container, identifier.hash_code(),
                                 onChild);
  } else if (child.has_display_area()) {
    protos::pbzero::DisplayAreaProto::Decoder display_area(
        child.display_area());
    protos::pbzero::WindowContainerProto::Decoder window_container(
        display_area.window_container());
    protos::pbzero::IdentifierProto::Decoder identifier(
        window_container.identifier());
    ParseWindowContainerChildren(window_container, identifier.hash_code(),
                                 onChild);
  } else if (child.has_task()) {
    protos::pbzero::TaskProto::Decoder task(child.task());
    protos::pbzero::WindowContainerProto::Decoder task_window_container(
        task.window_container());
    protos::pbzero::TaskFragmentProto::Decoder task_fragment(
        task.task_fragment());
    protos::pbzero::WindowContainerProto::Decoder
        task_fragment_window_container(task_fragment.window_container());

    protos::pbzero::WindowContainerProto::Decoder& identifier_window_container =
        task.has_task_fragment() && task_fragment.has_window_container()
            ? task_fragment_window_container
            : task_window_container;

    protos::pbzero::IdentifierProto::Decoder identifier(
        identifier_window_container.identifier());
    int32_t task_token = identifier.hash_code();

    protos::pbzero::WindowContainerProto::Decoder& window_container =
        task_fragment_window_container.has_children()
            ? task_fragment_window_container
            : task_window_container;

    ParseWindowContainerChildren(window_container, task_token, onChild);
  } else if (child.has_activity()) {
    protos::pbzero::ActivityRecordProto::Decoder activity(child.activity());
    protos::pbzero::WindowTokenProto::Decoder window_token(
        activity.window_token());
    int32_t current_token = window_token.hash_code();
    protos::pbzero::WindowContainerProto::Decoder window_container(
        window_token.window_container());
    ParseWindowContainerChildren(window_container, current_token, onChild);
  } else if (child.has_window_token()) {
    protos::pbzero::WindowTokenProto::Decoder window_token(
        child.window_token());
    int32_t current_token = window_token.hash_code();
    protos::pbzero::WindowContainerProto::Decoder window_container(
        window_token.window_container());
    ParseWindowContainerChildren(window_container, current_token, onChild);
  } else if (child.has_window()) {
    protos::pbzero::WindowStateProto::Decoder window_state(child.window());
    protos::pbzero::WindowContainerProto::Decoder window_container(
        window_state.window_container());
    protos::pbzero::IdentifierProto::Decoder identifier(
        window_container.identifier());
    ParseWindowContainerChildren(window_container, identifier.hash_code(),
                                 onChild);
  } else if (child.has_task_fragment()) {
    protos::pbzero::TaskFragmentProto::Decoder task_fragment(
        child.task_fragment());
    protos::pbzero::WindowContainerProto::Decoder window_container(
        task_fragment.window_container());
    protos::pbzero::IdentifierProto::Decoder identifier(
        window_container.identifier());
    ParseWindowContainerChildren(window_container, identifier.hash_code(),
                                 onChild);
  }
}

IterateWalkStrategy::IterateWalkStrategy() = default;
IterateWalkStrategy::~IterateWalkStrategy() = default;

protos::pbzero::WindowContainerProto::Decoder
IterateWalkStrategy::GetWindowContainer(
    const protos::pbzero::WindowContainerChildProto::Decoder& child) const {
  if (child.has_window_container()) {
    return protos::pbzero::WindowContainerProto::Decoder(
        child.window_container());
  }
  if (child.has_display_content()) {
    protos::pbzero::DisplayContentProto::Decoder display_content(
        child.display_content());
    protos::pbzero::DisplayAreaProto::Decoder display_area(
        display_content.root_display_area());
    return protos::pbzero::WindowContainerProto::Decoder(
        display_area.window_container());
  }
  if (child.has_display_area()) {
    protos::pbzero::DisplayAreaProto::Decoder display_area(
        child.display_area());
    return protos::pbzero::WindowContainerProto::Decoder(
        display_area.window_container());
  }
  if (child.has_task()) {
    protos::pbzero::TaskProto::Decoder task(child.task());
    protos::pbzero::TaskFragmentProto::Decoder task_fragment(
        task.task_fragment());
    if (task.has_task_fragment() && task_fragment.has_window_container()) {
      return protos::pbzero::WindowContainerProto::Decoder(
          task_fragment.window_container());
    }
    return protos::pbzero::WindowContainerProto::Decoder(
        task.window_container());
  }
  if (child.has_activity()) {
    protos::pbzero::ActivityRecordProto::Decoder activity(child.activity());
    protos::pbzero::WindowTokenProto::Decoder window_token(
        activity.window_token());
    return protos::pbzero::WindowContainerProto::Decoder(
        window_token.window_container());
  }
  if (child.has_window_token()) {
    protos::pbzero::WindowTokenProto::Decoder window_token(
        child.window_token());
    return protos::pbzero::WindowContainerProto::Decoder(
        window_token.window_container());
  }
  if (child.has_window()) {
    protos::pbzero::WindowStateProto::Decoder window_state(child.window());
    return protos::pbzero::WindowContainerProto::Decoder(
        window_state.window_container());
  }
  if (child.has_task_fragment()) {
    protos::pbzero::TaskFragmentProto::Decoder task_fragment(
        child.task_fragment());
    return protos::pbzero::WindowContainerProto::Decoder(
        task_fragment.window_container());
  }
  return protos::pbzero::WindowContainerProto::Decoder(nullptr, 0);
}

base::FlatHashMap<int32_t, IterateWalkStrategy::ParentLink>
IterateWalkStrategy::BuildChildToParentMap(
    const protos::pbzero::WindowManagerServiceDumpProto::Decoder& service)
    const {
  base::FlatHashMap<int32_t, ParentLink> child_to_parent;

  // TODO: adapt unit test (RootWindowContainerProto info entirely contained in
  // root_window_container field, no longer partially in window_containers flat
  // list (element without parent))
  protos::pbzero::RootWindowContainerProto::Decoder root(
      service.root_window_container());
  CollectParentLinks(
      protos::pbzero::WindowContainerProto::Decoder(root.window_container()),
      &child_to_parent);

  for (auto it = service.window_containers(); it; ++it) {
    protos::pbzero::WindowContainerChildProto::Decoder child(*it);
    CollectParentLinks(GetWindowContainer(child), &child_to_parent);
  }
  return child_to_parent;
}

void IterateWalkStrategy::CollectParentLinks(
    const protos::pbzero::WindowContainerProto::Decoder& window_container,
    base::FlatHashMap<int32_t, ParentLink>* child_to_parent) const {
  protos::pbzero::IdentifierProto::Decoder identifier(
      window_container.identifier());
  if (!identifier.has_hash_code()) {
    PERFETTO_DCHECK(false);  // TODO(keanmariotti): return
                             // base::ErrStatus(kErrorMessageMissingField);
  }
  int32_t token = identifier.hash_code();
  uint32_t index = 0;
  for (auto cit = window_container.child_tokens(); cit; ++cit) {
    int32_t child_token = *cit;
    (*child_to_parent)[child_token] = {token, index++};
  }
}

void IterateWalkStrategy::DispatchToCallbacks(
    const protos::pbzero::WindowManagerServiceDumpProto::Decoder& service,
    const base::FlatHashMap<int32_t, ParentLink>& child_to_parent,
    const std::function<
        void(const protos::pbzero::RootWindowContainerProto::Decoder&,
             const protos::pbzero::WindowContainerProto::Decoder&)>& onRoot,
    const std::function<
        void(const protos::pbzero::WindowContainerChildProto::Decoder&,
             int32_t parent_token,
             uint32_t child_index)>& onChild) const {
  // Root
  protos::pbzero::RootWindowContainerProto::Decoder root(
      service.root_window_container());
  // TODO(keanmariotti): get rid of second argument
  onRoot(root, protos::pbzero::WindowContainerProto::Decoder(
                   root.window_container()));

  // Children
  for (auto it = service.window_containers(); it; ++it) {
    protos::pbzero::WindowContainerChildProto::Decoder child(*it);
    int32_t token = child.token();

    auto* parent_info = child_to_parent.Find(token);
    if (!parent_info) {
      PERFETTO_DCHECK(false);  // TODO(keanmariotti): return error instead
    }
    onChild(child, parent_info->parent_token, parent_info->child_index);
  }
}

void IterateWalkStrategy::Walk(
    const protos::pbzero::WindowManagerTraceEntry::Decoder& entry,
    const std::function<
        void(const protos::pbzero::RootWindowContainerProto::Decoder&,
             const protos::pbzero::WindowContainerProto::Decoder&)>& onRoot,
    const std::function<
        void(const protos::pbzero::WindowContainerChildProto::Decoder&,
             int32_t parent_token,
             uint32_t child_index)>& onChild) {
  protos::pbzero::WindowManagerServiceDumpProto::Decoder service(
      entry.window_manager_service());

  auto child_to_parent = BuildChildToParentMap(service);

  DispatchToCallbacks(service, child_to_parent, onRoot, onChild);
}

}  // namespace perfetto::trace_processor::winscope
