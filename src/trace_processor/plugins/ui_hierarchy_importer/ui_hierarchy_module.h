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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_UI_HIERARCHY_IMPORTER_UI_HIERARCHY_MODULE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_UI_HIERARCHY_IMPORTER_UI_HIERARCHY_MODULE_H_

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/ui_hierarchy_importer/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Parses TracePacket.ui_hierarchy (android.ui.hierarchy data source) into:
//  - UiHierarchySnapshotTable: one row per packet.
//  - UiHierarchyWindowTable / UiHierarchyNodeTable: interval tables with one
//    row per distinct version of a window / node. Keyframes and deltas are
//    materialized so that every property change starts a new row and the full
//    tree at any time can be queried directly.
//
// Change detection hashes the *resolved* node content (interned strings are
// resolved to StringPool ids), so re-emitting an unchanged node in a keyframe
// or after an incremental-state reset does not create a new row.
class UiHierarchyModule : public ProtoImporterModule {
 public:
  UiHierarchyModule(ProtoImporterModuleContext* module_context,
                    TraceProcessorContext* context,
                    tables::UiHierarchySnapshotTable* snapshot_table,
                    tables::UiHierarchyWindowTable* window_table,
                    tables::UiHierarchyNodeTable* node_table,
                    tables::UiHierarchyWindowFrameTable* window_frame_table,
                    tables::UiHierarchyEventTable* event_table);
  ~UiHierarchyModule() override;

  void ParseField(const ParseFieldArgs& args) override;

 private:
  struct LiveRow {
    uint32_t row = 0;
    uint64_t hash = 0;
    // Set while processing a keyframe; rows not seen by the end of the
    // keyframe are closed.
    bool seen = false;
  };
  // A 3x3 row-major matrix mapping column vectors [x y 1].
  using Matrix = std::array<double, 9>;
  struct NodeState {
    NodeState() {}
    // Last emitted row and the hash of its full content (incl. derived
    // screen bounds).
    LiveRow live;
    // Hash of the recorded basis (content + local geometry), used to detect
    // whether a re-emitted node changed.
    uint64_t basis_hash = 0;
    // Recorded content (everything except the derived screen bounds).
    tables::UiHierarchyNodeTable::Row content;
    uint64_t content_hash = 0;
    std::optional<int64_t> parent;
    // Whether `parent` is reflected in WindowState::children.
    bool linked = false;
    Matrix local{};
    int32_t width = 0;
    int32_t height = 0;
    // Screen transform, valid when screen_gen == the current generation.
    Matrix screen{};
    uint64_t screen_gen = 0;
  };
  struct WindowState {
    WindowState() {}
    LiveRow window;
    int32_t left = 0;
    int32_t top = 0;
    base::FlatHashMap<int64_t, NodeState> nodes;
    base::FlatHashMap<int64_t, std::vector<int64_t>> children;
  };
  struct ProcessState {
    ProcessState() {}
    base::FlatHashMap<int64_t, WindowState> windows;
  };

  void ParseEvents(protozero::ConstBytes bytes,
                   std::optional<UniquePid> upid,
                   std::optional<int64_t> pid,
                   const TracePacketData& data);
  void ParseSnapshot(protozero::ConstBytes bytes,
                     int64_t ts,
                     std::optional<UniquePid> upid,
                     const TracePacketData& data);
  // Returns the number of nodes in the window message.
  uint32_t ParseWindow(protozero::ConstBytes bytes,
                       int64_t ts,
                       std::optional<UniquePid> upid,
                       ProcessState& ps,
                       const TracePacketData& data,
                       uint32_t* removed_count);
  // Records the basis of a node; returns true if it changed.
  bool ParseNode(protozero::ConstBytes bytes,
                 int64_t ts,
                 std::optional<UniquePid> upid,
                 int64_t window_id,
                 WindowState& ws,
                 const TracePacketData& data);
  // Recomputes the derived screen bounds of `dirty` nodes and all their
  // descendants, emitting new rows where the result changed.
  void RefreshNodes(WindowState& ws,
                    const std::vector<int64_t>& dirty,
                    int64_t ts);
  const Matrix& ScreenTransform(WindowState& ws, int64_t id, int depth);
  void SetParent(WindowState& ws, int64_t id, std::optional<int64_t> parent);
  void EraseNode(WindowState& ws, int64_t id, int64_t ts);

  void CloseNode(uint32_t row, int64_t ts);
  uint64_t generation_ = 0;
  void CloseWindow(WindowState& ws, int64_t ts);

  StringId ResolveString(const TracePacketData& data, uint64_t iid);

  TraceProcessorContext* const context_;
  tables::UiHierarchySnapshotTable* const snapshot_table_;
  tables::UiHierarchyWindowTable* const window_table_;
  tables::UiHierarchyNodeTable* const node_table_;
  tables::UiHierarchyWindowFrameTable* const window_frame_table_;
  tables::UiHierarchyEventTable* const event_table_;

  struct PendingFrame {
    int64_t window_id;
    std::optional<int64_t> frame_number;
    int64_t skipped_frames;
    bool removed;
    std::optional<int64_t> vsync_id;
    std::optional<int32_t> window_type;
  };
  // Window frame rows of the snapshot being parsed (inserted once the snapshot
  // row id is known).
  std::vector<PendingFrame> pending_frames_;

  // Scratch buffer for flattening repeated strings (reused to avoid
  // allocations).
  std::string string_buffer_;

  // Keyed by upid (or UINT32_MAX when the pid is unknown).
  base::FlatHashMap<uint32_t, ProcessState> processes_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_UI_HIERARCHY_IMPORTER_UI_HIERARCHY_MODULE_H_
