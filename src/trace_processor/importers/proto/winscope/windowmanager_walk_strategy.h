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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINDOWMANAGER_WALK_STRATEGY_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINDOWMANAGER_WALK_STRATEGY_H_

#include <functional>

#include "protos/perfetto/trace/android/server/windowmanagerservice.pbzero.h"
#include "protos/perfetto/trace/android/windowmanager.pbzero.h"

namespace perfetto::trace_processor::winscope {

class WalkStrategy {
 public:
  virtual void Walk(
      const protos::pbzero::WindowManagerTraceEntry::Decoder& entry,
      const std::function<
          void(const protos::pbzero::RootWindowContainerProto::Decoder&,
               const protos::pbzero::WindowContainerProto::Decoder&)>& onRoot,
      const std::function<
          void(const protos::pbzero::WindowContainerChildProto::Decoder&,
               int32_t parent_token,
               uint32_t child_index)>& onChild) = 0;
  virtual ~WalkStrategy();
};

class DfsWalkStrategy : public WalkStrategy {
 public:
  DfsWalkStrategy();
  ~DfsWalkStrategy() override;

  void Walk(const protos::pbzero::WindowManagerTraceEntry::Decoder& entry,
            const std::function<void(
                const protos::pbzero::RootWindowContainerProto::Decoder&,
                const protos::pbzero::WindowContainerProto::Decoder&)>& onRoot,
            const std::function<
                void(const protos::pbzero::WindowContainerChildProto::Decoder&,
                     int32_t parent_token,
                     uint32_t child_index)>& onChild) override;

 private:
  void ParseWindowContainerChildren(
      const protos::pbzero::WindowContainerProto::Decoder& window_container,
      int32_t parent_token,
      const std::function<
          void(const protos::pbzero::WindowContainerChildProto::Decoder&,
               int32_t parent_token,
               uint32_t child_index)>& onChild);

  void ParseWindowContainerChildProto(
      const protos::pbzero::WindowContainerChildProto::Decoder& child,
      int32_t parent_token,
      uint32_t index,
      const std::function<
          void(const protos::pbzero::WindowContainerChildProto::Decoder&,
               int32_t parent_token,
               uint32_t child_index)>& onChild);
};

}  // namespace perfetto::trace_processor::winscope

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINDOWMANAGER_WALK_STRATEGY_H_
