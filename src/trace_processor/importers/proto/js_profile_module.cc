// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <json/value.h>
#include <stdint.h>
#include <map>
#include <stack>
#include <string>
#include <vector>

#include "src/trace_processor/importers/proto/js_profile_module.h"

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/sorter/trace_sorter.h"

#include "src/trace_processor/importers/common/parser_types.h"

namespace perfetto {
namespace trace_processor {

enum ProfileEventType { BEGIN, END };
struct ProfileEvent {
  ProfileEventType type_;
  int32_t id_;
  int64_t timestamp_;
  ProfileEvent(ProfileEventType type, int32_t id, int64_t timestamp)
      : type_(type), id_(id), timestamp_(timestamp) {}
};

// Merge or create profile data in cache.
CpuProfileData MergeProfileData(
    int32_t profile_id,
    const protos::pbzero::JSProfilePacket_Decoder& decoder,
    base::FlatHashMap<int32_t, CpuProfileData>& cpu_profiles_) {
  CpuProfileData data;
  auto profile = cpu_profiles_.Find(profile_id);
  if (profile != nullptr) {
    profile->runtime_profile += decoder.runtime_profile().ToStdString();
    profile->is_done = decoder.is_done();
    data = *profile;
  } else {
    data.is_done = decoder.is_done();
    data.profile_id = decoder.profile_id();
    data.track_id = decoder.track_id();
    data.runtime_profile = decoder.runtime_profile().ToStdString();
    cpu_profiles_.Insert(profile_id, data);
  }
  return data;
}

// Parse 'samples' and 'timeDeltas' arrays from JSON.
bool ParseSamplesAndTimeDeltas(const Json::Value& runtime_profile,
                               CpuProfile& cpu_profile,
                               TraceProcessorContext* context_) {
  const Json::Value& samples = runtime_profile["samples"];
  const Json::Value& timeDeltas = runtime_profile["timeDeltas"];
  if (!samples.isArray() || !timeDeltas.isArray())
    return false;

  for (unsigned i = 0; i < samples.size(); i++) {
    cpu_profile.samples.push_back(samples[i].asInt());
  }
  for (unsigned i = 0; i < timeDeltas.size(); i++) {
    cpu_profile.time_deltas.push_back(timeDeltas[i].asInt64());
  }
  if (cpu_profile.time_deltas.size() != cpu_profile.samples.size()) {
    context_->storage->IncrementStats(stats::json_parser_failure);
    PERFETTO_ELOG(
        "TokenizeJsProfilePacket::json_parser_fail: samples size is not "
        "equal time_deltas size");
    return false;
  }
  return true;
}

// Parse profile JSON and fill basic information.
bool ParseProfileJson(const Json::Value& runtime_profile,
                      CpuProfile& cpu_profile,
                      TraceProcessorContext* context_) {
  cpu_profile.start_timestamp = runtime_profile["startTime"].asInt64();
  cpu_profile.end_timestamp = runtime_profile["endTime"].asInt64();
  return ParseSamplesAndTimeDeltas(runtime_profile, cpu_profile, context_);
}

// Parse 'nodes' array and build node map.
bool ParseNodes(const Json::Value& runtime_profile,
                CpuProfile& cpu_profile,
                std::map<int32_t, ProfileNode>& node_map) {
  const Json::Value& nodes = runtime_profile["nodes"];
  if (!nodes.isArray() || nodes.size() <= 0)
    return false;

  for (unsigned i = 0; i < nodes.size(); i++) {
    const Json::Value& node = nodes[i];
    const Json::Value& callFrame = node["callFrame"];
    const Json::Value& children = node["children"];
    ProfileNode profile_node;
    profile_node.id = node["id"].asInt();
    int32_t parent = -1;
    if (node.isMember("parent")) {
      parent = node["parent"].asInt();
    }
    profile_node.depth = -1;
    profile_node.parent = parent;
    if (callFrame.isObject()) {
      profile_node.call_frame.function_name =
          callFrame["functionName"].asString();
      if (profile_node.call_frame.function_name.empty()) {
        profile_node.call_frame.function_name = "(anonymous)";
      }
      profile_node.call_frame.column_number = callFrame["columnNumber"].asInt();
      profile_node.call_frame.line_number = callFrame["lineNumber"].asInt();
      profile_node.call_frame.url = callFrame["url"].asString();
    }
    if (children.isArray()) {
      for (unsigned j = 0; j < children.size(); j++) {
        profile_node.children.push_back(children[j].asInt());
      }
    }
    cpu_profile.nodes.push_back(profile_node);
    node_map.emplace(profile_node.id, profile_node);
  }
  return true;
}

// Find special node IDs: garbage collector, program, idle, and root.
void FindSpecialNodeIds(const std::map<int32_t, ProfileNode>& node_map,
                        int32_t& gcNodeId,
                        int32_t& programNodeId,
                        int32_t& idleNodeId,
                        int32_t& rootId) {
  gcNodeId = programNodeId = idleNodeId = rootId = -1;
  for (const auto& kv : node_map) {
    const auto& fn = kv.second.call_frame.function_name;
    if (fn == "(garbage collector)")
      gcNodeId = kv.first;
    if (fn == "(program)")
      programNodeId = kv.first;
    if (fn == "(idle)")
      idleNodeId = kv.first;
    if (fn == "(root)")
      rootId = kv.first;
  }
}

// Fix missing samples in the profile.
void FixMissingSamples(CpuProfile& cpu_profile,
                       int32_t programNodeId,
                       int32_t gcNodeId,
                       int32_t idleNodeId,
                       int32_t rootId,
                       const std::map<int32_t, ProfileNode>& node_map) {
  size_t samplesCount = cpu_profile.samples.size();
  if (programNodeId != -1 && samplesCount >= 3) {
    auto isSystemNode = [&programNodeId, &gcNodeId,
                         &idleNodeId](int32_t nodeId) {
      return nodeId == programNodeId || nodeId == gcNodeId ||
             nodeId == idleNodeId;
    };
    auto bottomNodeId = [&](int32_t nodeId) {
      for (auto node_it = node_map.find(nodeId); node_it != node_map.end();) {
        if (node_it->second.parent == -1 || node_it->second.parent == rootId) {
          break;
        }
        nodeId = node_it->second.parent;
        node_it = node_map.find(nodeId);
      }
      return nodeId;
    };
    int32_t prevId = cpu_profile.samples[0];
    int32_t nodeId = cpu_profile.samples[1];
    for (size_t index = 1; index < samplesCount - 1; index++) {
      const int32_t nextNodeId = cpu_profile.samples[index + 1];
      if (nodeId == programNodeId && !isSystemNode(prevId) &&
          !isSystemNode(nextNodeId) &&
          bottomNodeId(prevId) == bottomNodeId(nextNodeId)) {
        cpu_profile.samples[index] = prevId;
      }
      prevId = nodeId;
      nodeId = nextNodeId;
    }
  }
}

// Calculate depth and parent for each node.
void CalculateNodeDepthAndParent(std::map<int32_t, ProfileNode>& node_map) {
  for (auto it_node = node_map.begin(); it_node != node_map.end(); it_node++) {
    int32_t id = it_node->first;
    auto childs = it_node->second.children;
    for (auto it_child = childs.begin(); it_child != childs.end(); it_child++) {
      auto child = node_map.find(*it_child);
      int32_t depth = it_node->second.depth + 1;
      if (child != node_map.end()) {
        child->second.parent = id;
        child->second.depth = depth;
      }
    }
  }
}

// Generate a list of ProfileEvent from samples and nodes.
std::vector<ProfileEvent> GenerateProfileEvents(
    const CpuProfile& cpu_profile,
    const std::map<int32_t, ProfileNode>& node_map,
    int32_t gcNodeId) {
  std::vector<ProfileEvent> events;
  int64_t last_timestamp = cpu_profile.start_timestamp;
  int32_t prevId = -1;
  int32_t gcParentNodeId = -1;
  std::stack<int32_t> stackNodes;

  for (size_t i = 0; i < cpu_profile.samples.size(); i++) {
    int32_t id = cpu_profile.samples[i];
    last_timestamp += cpu_profile.time_deltas[i];
    if (last_timestamp > cpu_profile.end_timestamp)
      break;
    if (id == prevId)
      continue;
    if (prevId == -1 || id == gcNodeId) {
      events.emplace_back(ProfileEventType::BEGIN, id, last_timestamp);
      if (id == gcNodeId)
        gcParentNodeId = prevId;
      prevId = id;
      continue;
    }
    if (prevId == gcNodeId && gcParentNodeId != -1) {
      events.emplace_back(ProfileEventType::END, gcNodeId, last_timestamp);
      prevId = gcParentNodeId;
      gcParentNodeId = -1;
    }
    auto node_it = node_map.find(id);
    auto prev_it = node_map.find(prevId);
    if (node_it == node_map.end() || prev_it == node_map.end())
      break;
    auto prev_node = prev_it->second;
    auto node = node_it->second;
    int32_t node_id = id;
    // Traverse up the tree to align node depths
    while (node_id != -1 && node.depth > prev_node.depth) {
      stackNodes.push(node.id);
      int32_t parent_id = node.parent;
      node_id = parent_id;
      auto parent_it = node_map.find(parent_id);
      if (parent_it != node_map.end())
        node = parent_it->second;
    }
    // Close previous nodes until the current node is reached
    while (prev_node.id != node.id) {
      events.emplace_back(ProfileEventType::END, prev_node.id, last_timestamp);
      if (node.depth == prev_node.depth) {
        stackNodes.push(node.id);
        auto parent_it = node_map.find(node.parent);
        if (parent_it != node_map.end())
          node = parent_it->second;
        else
          break;
      }
      auto prev_parent_it = node_map.find(prev_node.parent);
      if (prev_parent_it == node_map.end())
        break;
      prev_node = prev_parent_it->second;
    }
    // Open nodes on the stack
    while (!stackNodes.empty()) {
      int32_t top_node_id = stackNodes.top();
      stackNodes.pop();
      events.emplace_back(ProfileEventType::BEGIN, top_node_id, last_timestamp);
    }
    prevId = id;
  }

  // Close any remaining open nodes
  for (auto prev_it = node_map.find(prevId); prev_it != node_map.end();) {
    events.emplace_back(ProfileEventType::END, prev_it->first, last_timestamp);
    prevId = prev_it->second.parent;
    prev_it = node_map.find(prevId);
  }
  return events;
}

// Emit ProfileEvents as TracePackets.
void EmitProfileEventsToTrace(const std::vector<ProfileEvent>& events,
                              const std::map<int32_t, ProfileNode>& node_map,
                              const CpuProfile& cpu_profile,
                              RefPtr<PacketSequenceStateGeneration> state,
                              TraceProcessorContext* context_) {
  std::stack<ProfileEvent> event_stack;
  for (const auto& event : events) {
    auto node = node_map.find(event.id_);
    // Filter out root, program, and lynx_core nodes
    if (node != node_map.end()) {
      if (node->second.call_frame.function_name == "(root)" ||
          node->second.call_frame.function_name == "(program)" ||
          node->second.call_frame.url.find("lynx_core") != std::string::npos) {
        continue;
      }
    }
    if (event.type_ == ProfileEventType::BEGIN) {
      event_stack.push(event);
    } else {
      auto end_node = node_map.find(event.id_);
      while (!event_stack.empty()) {
        int64_t count = static_cast<int64_t>(event_stack.size());
        auto start_event = event_stack.top();
        event_stack.pop();
        auto start_node = node_map.find(start_event.id_);
        if (start_node != node_map.end() && end_node != node_map.end()) {
          protozero::HeapBuffered<protos::pbzero::TracePacket> begin_packet;
          auto begin_event = begin_packet->set_track_event();
          begin_event->add_categories("jsprofile");
          begin_event->set_track_uuid(cpu_profile.track_id);
          auto begin_timestamp = start_event.timestamp_ * 1000 + count;
          begin_event->set_type(protos::pbzero::TrackEvent::TYPE_SLICE_BEGIN);
          begin_event->set_name(start_node->second.call_frame.function_name);
          auto* url_args = begin_event->add_debug_annotations();
          url_args->set_name("url");
          url_args->set_string_value(start_node->second.call_frame.url);
          auto* line_args = begin_event->add_debug_annotations();
          line_args->set_name("lineNumber");
          line_args->set_int_value(start_node->second.call_frame.line_number);
          auto* column_args = begin_event->add_debug_annotations();
          column_args->set_name("columnNumber");
          column_args->set_int_value(
              start_node->second.call_frame.column_number);
          std::vector<uint8_t> begin_vec = begin_packet.SerializeAsArray();
          std::unique_ptr<TrackEventData> begin_data(
              new TrackEventData(TraceBlobView(TraceBlob::CopyFrom(
                                     begin_vec.data(), begin_vec.size())),
                                 state));
          context_->sorter->PushTrackEventPacket(
              begin_timestamp, std::move(*begin_data), context_->machine_id());
          protozero::HeapBuffered<protos::pbzero::TracePacket> end_packet;
          auto end_event = end_packet->set_track_event();
          end_event->add_categories("jsprofile");
          end_event->set_track_uuid(cpu_profile.track_id);
          end_event->set_type(protos::pbzero::TrackEvent::TYPE_SLICE_END);
          end_event->set_name(start_node->second.call_frame.function_name);
          std::vector<uint8_t> end_vec = end_packet.SerializeAsArray();
          std::unique_ptr<TrackEventData> end_data(
              new TrackEventData(TraceBlobView(TraceBlob::CopyFrom(
                                     end_vec.data(), end_vec.size())),
                                 state));
          auto end_timestamp = event.timestamp_ * 1000 - count;
          context_->sorter->PushTrackEventPacket(
              end_timestamp, std::move(*end_data), context_->machine_id());
          if (event.id_ == start_event.id_) {
            break;
          }
        }
      }
    }
  }
}

using perfetto::protos::pbzero::TracePacket;

JSProfileModule::JSProfileModule(TraceProcessorContext* context)
    : context_(context) {
  RegisterForField(TracePacket::kJsProfilePacketFieldNumber, context);
  RegisterForField(TracePacket::kStatsdAtomFieldNumber, context);
}

ModuleResult JSProfileModule::TokenizePacket(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView* packet,
    int64_t,
    RefPtr<PacketSequenceStateGeneration> state,
    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kJsProfilePacketFieldNumber: {
      return this->TokenizeJsProfilePacket(state, decoder, packet);
    }
    // for old js profile field number
    case TracePacket::kStatsdAtomFieldNumber: {
      return this->TokenizeJsProfilePacketOld(state, decoder, packet);
    }
  }
  return ModuleResult::Ignored();
}

ModuleResult JSProfileModule::TokenizeJsProfilePacket(
    RefPtr<PacketSequenceStateGeneration> state,
    const protos::pbzero::TracePacket_Decoder& packet,
    TraceBlobView*) {
  using protos::pbzero::JSProfilePacket;
  auto field = packet.js_profile_packet();
  JSProfilePacket::Decoder decoder(field);

  return this->DecodeJsProfilePacket(state, decoder);
}

ModuleResult JSProfileModule::TokenizeJsProfilePacketOld(
    RefPtr<PacketSequenceStateGeneration> state,
    const protos::pbzero::TracePacket_Decoder& packet,
    TraceBlobView*) {
  using protos::pbzero::JSProfilePacket;
  // in old js profile package, we uses 84 as js profile packet field id which
  // conflicts with statsd_atom;
  auto field = packet.statsd_atom();
  JSProfilePacket::Decoder decoder(field);
  if (decoder.has_track_id()) {
    return this->DecodeJsProfilePacket(state, decoder);
  }
  return ModuleResult::Ignored();
}

ModuleResult JSProfileModule::DecodeJsProfilePacket(
    RefPtr<PacketSequenceStateGeneration> state,
    const protos::pbzero::JSProfilePacket_Decoder& decoder) {
  int32_t profile_id = decoder.profile_id();

  // Merge or create profile data
  CpuProfileData data = MergeProfileData(profile_id, decoder, cpu_profiles_);

  if (!data.is_done) {
    return ModuleResult::Ignored();
  }

  auto opt_value =
      json::ParseJsonString(base::StringView(data.runtime_profile));
  if (!opt_value) {
    context_->storage->IncrementStats(stats::json_parser_failure);
    PERFETTO_ELOG("TokenizeJsProfilePacket::json_parser_failure");
    return ModuleResult::Ignored();
  }
  const Json::Value& result = *opt_value;
  // Parse JSON to fill CpuProfile
  CpuProfile cpu_profile;
  cpu_profile.track_id = data.track_id;

  // Parse profile
  const Json::Value& runtime_profile = result["profile"];
  ParseProfileJson(runtime_profile, cpu_profile, context_);

  // Parse nodes and build node map
  std::map<int32_t, ProfileNode> node_map;
  if (!ParseNodes(runtime_profile, cpu_profile, node_map)) {
    return ModuleResult::Ignored();
  }

  // Find special node IDs
  int32_t gcNodeId, programNodeId, idleNodeId, rootId;
  FindSpecialNodeIds(node_map, gcNodeId, programNodeId, idleNodeId, rootId);

  // Fix missing samples
  FixMissingSamples(cpu_profile, programNodeId, gcNodeId, idleNodeId, rootId,
                    node_map);

  // Calculate node depth and parent
  CalculateNodeDepthAndParent(node_map);

  // Generate profile events
  std::vector<ProfileEvent> events =
      GenerateProfileEvents(cpu_profile, node_map, gcNodeId);

  // Emit trace events
  EmitProfileEventsToTrace(events, node_map, cpu_profile, state, context_);

  return ModuleResult::Ignored();
}

}  // namespace trace_processor
}  // namespace perfetto
