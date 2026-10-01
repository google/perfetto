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

#include "src/trace_processor/plugins/ui_hierarchy_importer/ui_hierarchy_module.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/fnv_hash.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/plugins/ui_hierarchy_importer/tables_py.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/android/ui_hierarchy.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

using ::perfetto::protos::pbzero::InternedData;
using ::perfetto::protos::pbzero::TracePacket;
using ::perfetto::protos::pbzero::UiEvent;
using ::perfetto::protos::pbzero::UiHierarchyEvents;
using ::perfetto::protos::pbzero::UiHierarchySnapshot;
using ::perfetto::protos::pbzero::UiNode;
using ::perfetto::protos::pbzero::UiProperty;
using ::perfetto::protos::pbzero::UiWindow;

namespace {

constexpr uint32_t kUnknownUpid = std::numeric_limits<uint32_t>::max();

void HashDouble(base::FnvHasher& h, double v) {
  uint64_t bits;
  static_assert(sizeof(bits) == sizeof(v));
  memcpy(&bits, &v, sizeof(v));
  h.Update(bits);
}

}  // namespace

UiHierarchyModule::UiHierarchyModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context,
    tables::UiHierarchySnapshotTable* snapshot_table,
    tables::UiHierarchyWindowTable* window_table,
    tables::UiHierarchyNodeTable* node_table,
    tables::UiHierarchyWindowFrameTable* window_frame_table,
    tables::UiHierarchyEventTable* event_table)
    : ProtoImporterModule(module_context),
      context_(context),
      snapshot_table_(snapshot_table),
      window_table_(window_table),
      node_table_(node_table),
      window_frame_table_(window_frame_table),
      event_table_(event_table) {
  RegisterForField(TracePacket::kUiHierarchyFieldNumber);
  RegisterForField(TracePacket::kUiHierarchyEventsFieldNumber);
}

UiHierarchyModule::~UiHierarchyModule() = default;

void UiHierarchyModule::ParseField(const ParseFieldArgs& args) {
  std::optional<UniquePid> upid;
  std::optional<int64_t> pid;
  if (args.decoder.has_trusted_pid()) {
    pid = args.decoder.trusted_pid();
    upid = context_->process_tracker->GetOrCreateProcess(*pid);
  }
  switch (args.field.id()) {
    case TracePacket::kUiHierarchyFieldNumber:
      ParseSnapshot(args.field.Cast<TracePacket::kUiHierarchy>(), args.ts, upid,
                    args.data);
      break;
    case TracePacket::kUiHierarchyEventsFieldNumber:
      ParseEvents(args.field.Cast<TracePacket::kUiHierarchyEvents>(), upid, pid,
                  args.data);
      break;
    default:
      break;
  }
}

StringId UiHierarchyModule::ResolveString(const TracePacketData& data,
                                          uint64_t iid) {
  if (iid == 0) {
    return kNullStringId;
  }
  std::optional<StringId> id = data.sequence_state->InternedStringId(
      InternedData::kUiStringsFieldNumber, iid);
  if (!id) {
    context_->stats_tracker->IncrementStats(
        stats::ui_hierarchy_missing_interned_string);
    return kNullStringId;
  }
  return *id;
}

void UiHierarchyModule::ParseEvents(protozero::ConstBytes bytes,
                                    std::optional<UniquePid> upid,
                                    std::optional<int64_t> pid,
                                    const TracePacketData& data) {
  UiHierarchyEvents::Decoder batch(bytes);
  for (auto it = batch.events(); it; ++it) {
    UiEvent::Decoder e(*it);
    tables::UiHierarchyEventTable::Row row;
    // Event timestamps are absolute, in the packet's (trace) clock domain.
    row.ts = e.ts();
    row.dur = e.dur();
    row.upid = upid;
    if (e.has_tid() && pid) {
      row.utid = context_->process_tracker->UpdateThread(e.tid(), *pid);
    }
    row.type = e.type();
    StringId name = ResolveString(data, e.name_iid());
    if (!name.is_null())
      row.name = name;
    if (e.has_scope_id())
      row.scope_id = e.scope_id();
    if (e.has_state_id())
      row.state_id = e.state_id();
    StringId value = ResolveString(data, e.value_iid());
    if (!value.is_null())
      row.value = value;
    row.depth = static_cast<int32_t>(e.depth());
    if (e.has_dirty1())
      row.dirty1 = e.dirty1();
    if (e.has_dirty2())
      row.dirty2 = e.dirty2();
    if (e.has_composition_id())
      row.composition_id = e.composition_id();
    if (e.has_node_id())
      row.node_id = e.node_id();
    if (e.has_parent_id())
      row.parent_id = e.parent_id();
    if (e.has_index())
      row.node_index = e.index();
    if (e.has_from_index())
      row.from_index = e.from_index();
    if (e.has_x())
      row.x = static_cast<double>(e.x());
    if (e.has_y())
      row.y = static_cast<double>(e.y());
    if (e.has_width())
      row.width = e.width();
    if (e.has_height())
      row.height = e.height();
    if (e.has_min_width())
      row.min_width = e.min_width();
    if (e.has_max_width())
      row.max_width = e.max_width();
    if (e.has_min_height())
      row.min_height = e.min_height();
    if (e.has_max_height())
      row.max_height = e.max_height();
    if (e.has_object_id())
      row.object_id = e.object_id();
    if (e.has_frame_time_ns())
      row.frame_time_ns = e.frame_time_ns();
    if (e.has_play_time_ns())
      row.play_time_ns = e.play_time_ns();
    if (e.has_pointer_id())
      row.pointer_id = e.pointer_id();
    if (e.has_action())
      row.action = e.action();
    if (e.has_consumed())
      row.consumed = e.consumed() ? 1u : 0u;
    if (e.has_delta())
      row.delta = static_cast<double>(e.delta());
    if (e.has_consumed_delta())
      row.consumed_delta = static_cast<double>(e.consumed_delta());
    if (e.has_window_id())
      row.window_id = e.window_id();
    if (e.has_input_event_id())
      row.input_event_id = e.input_event_id();
    if (e.has_is_lookahead())
      row.is_lookahead = e.is_lookahead() ? 1u : 0u;
    StringId spec = ResolveString(data, e.spec_iid());
    if (!spec.is_null())
      row.spec = spec;
    event_table_->Insert(row);
  }
}

void UiHierarchyModule::ParseSnapshot(protozero::ConstBytes bytes,
                                      int64_t ts,
                                      std::optional<UniquePid> upid,
                                      const TracePacketData& data) {
  UiHierarchySnapshot::Decoder snap(bytes);
  const bool is_keyframe = snap.is_keyframe();
  ProcessState& ps = processes_[upid.value_or(kUnknownUpid)];

  if (is_keyframe) {
    for (auto it = snap.windows(); it; ++it) {
      UiWindow::Decoder win(*it);
      if (WindowState* ws = ps.windows.Find(win.id())) {
        for (auto nit = ws->nodes.GetIterator(); nit; ++nit) {
          nit.value().live.seen = false;
        }
      }
    }
  }

  uint32_t changed = 0;
  uint32_t removed = 0;
  for (auto it = snap.windows(); it; ++it) {
    changed += ParseWindow(*it, ts, upid, ps, data, &removed);
  }

  if (is_keyframe) {
    // Within windows present in this keyframe, nodes not re-emitted are gone.
    for (auto it = snap.windows(); it; ++it) {
      UiWindow::Decoder win(*it);
      if (WindowState* ws = ps.windows.Find(win.id())) {
        std::vector<int64_t> dead_nodes;
        for (auto nit = ws->nodes.GetIterator(); nit; ++nit) {
          if (!nit.value().live.seen) {
            dead_nodes.push_back(nit.key());
          }
        }
        removed += static_cast<uint32_t>(dead_nodes.size());
        for (int64_t id : dead_nodes) {
          EraseNode(*ws, id, ts);
        }
      }
    }
  }

  tables::UiHierarchySnapshotTable::Row row;
  row.ts = ts;
  row.upid = upid;
  row.is_keyframe = is_keyframe ? 1 : 0;
  if (snap.has_vsync_id()) {
    row.vsync_id = snap.vsync_id();
  }
  if (snap.has_effective_text_mode()) {
    row.text_mode = snap.effective_text_mode();
  }
  row.changed_node_count = static_cast<int32_t>(changed);
  row.removed_node_count = static_cast<int32_t>(removed);
  auto snapshot_id = snapshot_table_->Insert(row).id;
  for (const PendingFrame& f : pending_frames_) {
    tables::UiHierarchyWindowFrameTable::Row fr;
    fr.ts = ts;
    fr.upid = upid;
    fr.snapshot_id = snapshot_id;
    fr.window_id = f.window_id;
    fr.frame_number = f.frame_number;
    fr.skipped_frames = f.skipped_frames;
    fr.is_removed = f.removed ? 1 : 0;
    fr.vsync_id = f.vsync_id;
    fr.window_type = f.window_type;
    window_frame_table_->Insert(fr);
  }
  pending_frames_.clear();
}

uint32_t UiHierarchyModule::ParseWindow(protozero::ConstBytes bytes,
                                        int64_t ts,
                                        std::optional<UniquePid> upid,
                                        ProcessState& ps,
                                        const TracePacketData& data,
                                        uint32_t* removed_count) {
  UiWindow::Decoder win(bytes);
  const int64_t window_id = win.id();

  pending_frames_.push_back(
      {window_id,
       win.has_frame_number()
           ? std::make_optional(static_cast<int64_t>(win.frame_number()))
           : std::nullopt,
       static_cast<int64_t>(win.skipped_frames()), win.removed(),
       win.has_vsync_id() ? std::make_optional(win.vsync_id()) : std::nullopt,
       win.has_window_type() ? std::make_optional(win.window_type())
                             : std::nullopt});

  if (win.removed()) {
    if (WindowState* ws = ps.windows.Find(window_id)) {
      *removed_count += static_cast<uint32_t>(ws->nodes.size());
      CloseWindow(*ws, ts);
      ps.windows.Erase(window_id);
    }
    return 0;
  }

  auto [ws_ptr, inserted] = ps.windows.Insert(window_id, WindowState());
  WindowState& ws = *ws_ptr;

  // Window properties are always sent in full with each window message.
  StringId title = ResolveString(data, win.title_iid());
  base::FnvHasher h;
  h.UpdateAll(title.raw_id(), win.display_id(), win.left(), win.top(),
              win.right(), win.bottom(), win.has_focus());
  const uint64_t hash = h.digest();
  // Window roots are positioned relative to the window, so moving the window
  // moves every node.
  const bool window_moved =
      !inserted && (ws.left != win.left() || ws.top != win.top());
  ws.left = win.left();
  ws.top = win.top();
  if (inserted || ws.window.hash != hash) {
    if (!inserted) {
      auto rr = (*window_table_)[ws.window.row];
      rr.set_dur(ts - rr.ts());
    }
    tables::UiHierarchyWindowTable::Row row;
    row.ts = ts;
    row.dur = -1;
    row.upid = upid;
    row.window_id = window_id;
    if (!title.is_null())
      row.title = title;
    row.display_id = win.display_id();
    row.bounds_left = win.left();
    row.bounds_top = win.top();
    row.bounds_right = win.right();
    row.bounds_bottom = win.bottom();
    row.has_focus = win.has_focus() ? 1 : 0;
    ws.window.row = window_table_->Insert(row).row;
    ws.window.hash = hash;
  }
  ws.window.seen = true;

  uint32_t node_count = 0;
  std::vector<int64_t> dirty;
  for (auto it = win.nodes(); it; ++it) {
    UiNode::Decoder n(*it);
    if (ParseNode(*it, ts, upid, window_id, ws, data)) {
      dirty.push_back(n.id());
    }
    ++node_count;
  }
  for (auto it = win.removed_node_ids(); it; ++it) {
    int64_t id = *it;
    if (ws.nodes.Find(id)) {
      EraseNode(ws, id, ts);
      ++*removed_count;
    }
  }
  if (window_moved) {
    dirty.clear();
    for (auto it = ws.nodes.GetIterator(); it; ++it) {
      dirty.push_back(it.key());
    }
  }
  RefreshNodes(ws, dirty, ts);
  return node_count;
}

bool UiHierarchyModule::ParseNode(protozero::ConstBytes bytes,
                                  int64_t ts,
                                  std::optional<UniquePid> upid,
                                  int64_t window_id,
                                  WindowState& ws,
                                  const TracePacketData& data) {
  UiNode::Decoder node(bytes);

  tables::UiHierarchyNodeTable::Row row;
  row.ts = ts;
  row.dur = -1;
  row.upid = upid;
  row.window_id = window_id;
  row.node_id = node.id();
  if (node.has_parent_id()) {
    row.parent_node_id = node.parent_id();
  }
  row.child_index = node.index();
  row.kind = node.kind();
  const StringId name = ResolveString(data, node.name_iid());
  const StringId source_location =
      ResolveString(data, node.source_location_iid());
  const StringId text = ResolveString(data, node.text_iid());
  const StringId content_description =
      ResolveString(data, node.content_description_iid());
  const StringId test_tag = ResolveString(data, node.test_tag_iid());
  const StringId role = ResolveString(data, node.role_iid());
  const StringId state_description =
      ResolveString(data, node.state_description_iid());
  if (!name.is_null())
    row.name = name;
  if (!source_location.is_null())
    row.source_location = source_location;
  if (!text.is_null())
    row.text = text;
  if (!content_description.is_null())
    row.content_description = content_description;
  if (!test_tag.is_null())
    row.test_tag = test_tag;
  if (!role.is_null())
    row.role = role;
  if (!state_description.is_null())
    row.state_description = state_description;
  Matrix local = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  if (node.has_transform()) {
    size_t i = 0;
    std::string str;
    bool parse_error = false;
    for (auto it = node.transform(&parse_error); it && i < 9; ++it, ++i) {
      local[i] = static_cast<double>(*it);
      if (!str.empty())
        str += " ";
      str += std::to_string(*it);
    }
    if (i != 9 || parse_error) {
      context_->stats_tracker->IncrementStats(
          stats::ui_hierarchy_invalid_transform);
      local = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    } else {
      row.transform = context_->storage->InternString(base::StringView(str));
    }
  }
  if (!row.transform) {
    local[2] = static_cast<double>(node.x());
    local[5] = static_cast<double>(node.y());
  }
  row.local_x = static_cast<double>(node.x());
  row.local_y = static_cast<double>(node.y());
  row.width = node.width();
  row.height = node.height();
  row.alpha = node.has_alpha() ? static_cast<double>(node.alpha()) : 1.0;
  row.flags = static_cast<int64_t>(node.flags());
  if (node.has_elevation()) {
    row.elevation = static_cast<double>(node.elevation());
  }
  if (node.has_scroll_x())
    row.scroll_x = node.scroll_x();
  if (node.has_scroll_y())
    row.scroll_y = node.scroll_y();
  if (node.has_lookahead_x())
    row.lookahead_x = node.lookahead_x();
  if (node.has_lookahead_y())
    row.lookahead_y = node.lookahead_y();
  if (node.has_lookahead_width())
    row.lookahead_width = node.lookahead_width();
  if (node.has_lookahead_height())
    row.lookahead_height = node.lookahead_height();
  if (node.has_hashcode())
    row.hashcode = node.hashcode();
  if (node.has_clip_bounds()) {
    int32_t cb[4] = {0, 0, 0, 0};
    size_t i = 0;
    bool parse_error = false;
    for (auto it = node.clip_bounds(&parse_error); it && i < 4; ++it, ++i) {
      cb[i] = *it;
    }
    if (i == 4 && !parse_error) {
      const bool non_empty = cb[2] > cb[0] && cb[3] > cb[1];
      row.clip_left = non_empty ? cb[0] + ws.left : 0;
      row.clip_top = non_empty ? cb[1] + ws.top : 0;
      row.clip_right = non_empty ? cb[2] + ws.left : 0;
      row.clip_bottom = non_empty ? cb[3] + ws.top : 0;
    }
  }
  if (node.has_draw_order())
    row.draw_order = node.draw_order();
  if (node.has_z())
    row.z = static_cast<double>(node.z());

  base::FnvHasher h;
  h.UpdateAll(row.parent_node_id.value_or(-1), row.child_index, row.kind,
              name.raw_id(), source_location.raw_id(), row.flags, text.raw_id(),
              content_description.raw_id(), test_tag.raw_id(), role.raw_id(),
              state_description.raw_id(), row.width, row.height,
              row.scroll_x.value_or(0), row.scroll_y.value_or(0),
              row.lookahead_x.value_or(-1), row.lookahead_y.value_or(-1),
              row.lookahead_width.value_or(-1),
              row.lookahead_height.value_or(-1), row.hashcode.value_or(0),
              row.clip_left.value_or(-1), row.clip_top.value_or(-1),
              row.clip_right.value_or(-1), row.clip_bottom.value_or(-1),
              row.draw_order.value_or(-1));
  HashDouble(h, row.alpha);
  HashDouble(h, row.elevation.value_or(-1));
  HashDouble(h, row.z.value_or(-1));
  for (double v : local) {
    HashDouble(h, v);
  }

  // Repeated string lists are flattened into a single string column.
  // TODO(ui_hierarchy): consider exposing properties via arg sets instead.
  if (node.has_action_iids()) {
    string_buffer_.clear();
    for (auto it = node.action_iids(); it; ++it) {
      StringId id = ResolveString(data, *it);
      h.Update(id.raw_id());
      if (id.is_null())
        continue;
      if (!string_buffer_.empty())
        string_buffer_ += ",";
      string_buffer_ += context_->storage->GetString(id).ToStdString();
    }
    row.actions =
        context_->storage->InternString(base::StringView(string_buffer_));
  }
  if (node.has_properties()) {
    string_buffer_.clear();
    for (auto it = node.properties(); it; ++it) {
      UiProperty::Decoder p(*it);
      StringId k = ResolveString(data, p.name_iid());
      StringId v = ResolveString(data, p.value_iid());
      h.UpdateAll(k.raw_id(), v.raw_id());
      if (!string_buffer_.empty())
        string_buffer_ += "\n";
      string_buffer_ += context_->storage->GetString(k).ToStdString();
      string_buffer_ += "=";
      string_buffer_ += context_->storage->GetString(v).ToStdString();
    }
    row.properties =
        context_->storage->InternString(base::StringView(string_buffer_));
  }
  const uint64_t hash = h.digest();

  auto [ns, inserted] = ws.nodes.Insert(row.node_id, NodeState{});
  ns->live.seen = true;
  if (!inserted && ns->basis_hash == hash) {
    return false;  // Unchanged basis.
  }
  ns->basis_hash = hash;
  ns->content = std::move(row);
  ns->local = local;
  ns->width = ns->content.width;
  ns->height = ns->content.height;
  SetParent(ws, ns->content.node_id, ns->content.parent_node_id);
  return true;
}

void UiHierarchyModule::SetParent(WindowState& ws,
                                  int64_t id,
                                  std::optional<int64_t> parent) {
  NodeState* ns = ws.nodes.Find(id);
  if (!ns || (ns->linked && ns->parent == parent)) {
    return;
  }
  if (ns->linked && ns->parent) {
    if (auto* siblings = ws.children.Find(*ns->parent)) {
      for (size_t i = 0; i < siblings->size(); ++i) {
        if ((*siblings)[i] == id) {
          (*siblings)[i] = siblings->back();
          siblings->pop_back();
          break;
        }
      }
    }
  }
  ns->parent = parent;
  ns->linked = true;
  if (parent) {
    ws.children[*parent].push_back(id);
  }
}

void UiHierarchyModule::EraseNode(WindowState& ws, int64_t id, int64_t ts) {
  NodeState* ns = ws.nodes.Find(id);
  if (!ns) {
    return;
  }
  if (ns->live.hash != 0) {
    CloseNode(ns->live.row, ts);
  }
  SetParent(ws, id, std::nullopt);
  ws.nodes.Erase(id);
}

const UiHierarchyModule::Matrix&
UiHierarchyModule::ScreenTransform(WindowState& ws, int64_t id, int depth) {
  NodeState* ns = ws.nodes.Find(id);
  PERFETTO_DCHECK(ns);
  if (ns->screen_gen == generation_) {
    return ns->screen;
  }
  Matrix parent = {1, 0, static_cast<double>(ws.left),
                   0, 1, static_cast<double>(ws.top),
                   0, 0, 1};
  // Depth guard against malformed (cyclic) parent links.
  if (ns->parent && depth < 512 && ws.nodes.Find(*ns->parent)) {
    parent = ScreenTransform(ws, *ns->parent, depth + 1);
  }
  const Matrix& l = ns->local;
  Matrix r;
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      r[i * 3 + j] = parent[i * 3 + 0] * l[0 * 3 + j] +
                     parent[i * 3 + 1] * l[1 * 3 + j] +
                     parent[i * 3 + 2] * l[2 * 3 + j];
    }
  }
  ns->screen = r;
  ns->screen_gen = generation_;
  return ns->screen;
}

void UiHierarchyModule::RefreshNodes(WindowState& ws,
                                     const std::vector<int64_t>& dirty,
                                     int64_t ts) {
  if (dirty.empty()) {
    return;
  }
  ++generation_;
  // Collect dirty nodes and all their descendants: a parent's geometry change
  // moves every descendant on screen without the producer re-emitting them.
  std::vector<int64_t> todo;
  base::FlatHashMap<int64_t, bool> visited;
  std::vector<int64_t> stack(dirty.begin(), dirty.end());
  while (!stack.empty()) {
    int64_t id = stack.back();
    stack.pop_back();
    if (!ws.nodes.Find(id) || !visited.Insert(id, true).second) {
      continue;
    }
    todo.push_back(id);
    if (auto* kids = ws.children.Find(id)) {
      stack.insert(stack.end(), kids->begin(), kids->end());
    }
  }
  for (int64_t id : todo) {
    const Matrix m = ScreenTransform(ws, id, 0);
    NodeState* ns = ws.nodes.Find(id);
    const double w = ns->width;
    const double hgt = ns->height;
    const double corners[4][2] = {{0, 0}, {w, 0}, {0, hgt}, {w, hgt}};
    double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    for (size_t c = 0; c < 4; ++c) {
      double x = m[0] * corners[c][0] + m[1] * corners[c][1] + m[2];
      double y = m[3] * corners[c][0] + m[4] * corners[c][1] + m[5];
      double z = m[6] * corners[c][0] + m[7] * corners[c][1] + m[8];
      if (std::abs(z) > 1e-12 && std::abs(z - 1.0) > 1e-12) {
        x /= z;
        y /= z;
      }
      if (c == 0 || x < min_x)
        min_x = x;
      if (c == 0 || y < min_y)
        min_y = y;
      if (c == 0 || x > max_x)
        max_x = x;
      if (c == 0 || y > max_y)
        max_y = y;
    }
    tables::UiHierarchyNodeTable::Row row = ns->content;
    row.ts = ts;
    row.dur = -1;
    row.bounds_left = static_cast<int32_t>(std::lround(min_x));
    row.bounds_top = static_cast<int32_t>(std::lround(min_y));
    row.bounds_right = static_cast<int32_t>(std::lround(max_x));
    row.bounds_bottom = static_cast<int32_t>(std::lround(max_y));

    base::FnvHasher h;
    h.UpdateAll(ns->basis_hash, row.bounds_left, row.bounds_top,
                row.bounds_right, row.bounds_bottom);
    uint64_t hash = h.digest();
    if (hash == 0)
      hash = 1;  // 0 means "no row emitted yet".
    if (ns->live.hash == hash) {
      continue;
    }
    if (ns->live.hash != 0) {
      CloseNode(ns->live.row, ts);
    }
    ns->live.row = node_table_->Insert(row).row;
    ns->live.hash = hash;
  }
}

void UiHierarchyModule::CloseNode(uint32_t row, int64_t ts) {
  auto rr = (*node_table_)[row];
  if (rr.dur() == -1) {
    rr.set_dur(ts - rr.ts());
  }
}

void UiHierarchyModule::CloseWindow(WindowState& ws, int64_t ts) {
  for (auto it = ws.nodes.GetIterator(); it; ++it) {
    if (it.value().live.hash != 0) {
      CloseNode(it.value().live.row, ts);
    }
  }
  auto rr = (*window_table_)[ws.window.row];
  if (rr.dur() == -1) {
    rr.set_dur(ts - rr.ts());
  }
}

}  // namespace perfetto::trace_processor
