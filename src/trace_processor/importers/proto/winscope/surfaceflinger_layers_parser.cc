/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_parser.h"
#include <cstdint>
#include <optional>
#include <unordered_set>

#include "perfetto/ext/base/base64.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layer_extractor.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layer_utils.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_rect_computation.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_visibility_computation.h"
#include "src/trace_processor/importers/proto/winscope/winscope_rect.h"
#include "src/trace_processor/importers/proto/winscope/winscope_transform.h"
#include "src/trace_processor/tables/winscope_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/proto_to_args_parser.h"
#include "src/trace_processor/util/winscope_proto_mapping.h"

namespace perfetto {
namespace trace_processor {

using VisibilityProperties = SfVisibilityComputation::VisibilityProperties;

SurfaceFlingerLayersParser::SurfaceFlingerLayersParser(
    TraceProcessorContext* context)
    : context_{context}, args_parser_{*context->descriptor_pool_} {}

void SurfaceFlingerLayersParser::Parse(int64_t timestamp,
                                       protozero::ConstBytes blob,
                                       std::optional<uint32_t> sequence_id) {
  protos::pbzero::LayersSnapshotProto::Decoder snapshot_decoder(blob);

  const auto& snapshot_id = ParseSnapshot(timestamp, blob, sequence_id);

  std::unordered_map<uint32_t, WinscopeRect> displays_by_layer_stack;

  if (snapshot_decoder.has_displays()) {
    int index = 0;
    for (auto it = snapshot_decoder.displays(); it; ++it) {
      protos::pbzero::DisplayProto::Decoder display_decoder(*it);
      ParseDisplay(display_decoder, snapshot_id, index,
                   displays_by_layer_stack);
      index++;
    }
  }

  protos::pbzero::LayersProto::Decoder layers_decoder(
      snapshot_decoder.layers());

  const std::unordered_map<int, protozero::ConstBytes>& layers_by_id =
      SfLayerExtractor::ExtractLayersById(layers_decoder);

  const std::vector<protozero::ConstBytes>& layers_top_to_bottom =
      SfLayerExtractor::ExtractLayersTopToBottom(layers_decoder);

  std::unordered_map<int, VisibilityProperties> computed_visibility =
      SfVisibilityComputation{}
          .setSnapshot(snapshot_decoder)
          ->setLayersTopToBottom(layers_top_to_bottom)
          ->setLayersById(&layers_by_id)
          ->Compute();

  auto* rect_tracker_ = WinscopeRectTracker::GetOrCreate(context_);
  auto* transform_tracker_ = WinscopeTransformTracker::GetOrCreate(context_);

  const auto& layer_rects = SfRectComputation{}
                                .setSnapshot(snapshot_decoder)
                                ->setLayers(layers_top_to_bottom)
                                ->setComputedVisibility(computed_visibility)
                                ->setDisplays(displays_by_layer_stack)
                                ->setRectTracker(rect_tracker_)
                                ->setTransformTracker(transform_tracker_)
                                ->Compute();

  for (auto it = layers_decoder.layers(); it; ++it) {
    protos::pbzero::LayerProto::Decoder layer(*it);
    std::optional<VisibilityProperties> visibility;
    SfRectComputation::SurfaceFlingerRects rects;
    if (layer.has_id()) {
      auto maybe_visibility = computed_visibility.find(layer.id());
      if (maybe_visibility != computed_visibility.end()) {
        visibility = maybe_visibility->second;
      }
      auto maybe_rects = layer_rects.find(layer.id());
      if (maybe_rects != layer_rects.end()) {
        rects = maybe_rects->second;
      }
    }

    ParseLayer(timestamp, *it, snapshot_id, visibility, layers_by_id, rects);
  }
}

const tables::SurfaceFlingerLayersSnapshotTable::Id
SurfaceFlingerLayersParser::ParseSnapshot(int64_t timestamp,
                                          protozero::ConstBytes blob,
                                          std::optional<uint32_t> sequence_id) {
  tables::SurfaceFlingerLayersSnapshotTable::Row snapshot;
  snapshot.ts = timestamp;
  snapshot.base64_proto_id = context_->storage->mutable_string_pool()
                                 ->InternString(base::StringView(
                                     base::Base64Encode(blob.data, blob.size)))
                                 .raw_id();
  if (sequence_id) {
    snapshot.sequence_id = *sequence_id;
  }
  const auto snapshot_id =
      context_->storage->mutable_surfaceflinger_layers_snapshot_table()
          ->Insert(snapshot)
          .id;

  auto inserter = context_->args_tracker->AddArgsTo(snapshot_id);
  ArgsParser writer(timestamp, inserter, *context_->storage);
  const auto table_name = tables::SurfaceFlingerLayersSnapshotTable::Name();
  auto allowed_fields =
      util::winscope_proto_mapping::GetAllowedFields(table_name);
  base::Status status = args_parser_.ParseMessage(
      blob, *util::winscope_proto_mapping::GetProtoName(table_name),
      &allowed_fields.value(), writer);
  if (!status.ok()) {
    context_->storage->IncrementStats(stats::winscope_sf_layers_parse_errors);
  }
  return snapshot_id;
}

void SurfaceFlingerLayersParser::ParseLayer(
    int64_t timestamp,
    protozero::ConstBytes blob,
    tables::SurfaceFlingerLayersSnapshotTable::Id snapshot_id,
    const std::optional<VisibilityProperties>& visibility,
    const std::unordered_map<int, protozero::ConstBytes>& layers_by_id,
    const SfRectComputation::SurfaceFlingerRects& rects) {
  ArgsTracker tracker(context_);
  auto row_id =
      InsertLayerRow(blob, snapshot_id, visibility, layers_by_id, rects);
  auto inserter = tracker.AddArgsTo(row_id);
  ArgsParser writer(timestamp, inserter, *context_->storage);
  base::Status status =
      args_parser_.ParseMessage(blob,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::SurfaceFlingerLayerTable::Name()),
                                nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    context_->storage->IncrementStats(stats::winscope_sf_layers_parse_errors);
  }

  if (!visibility.has_value()) {
    return;
  }
  if (visibility->visibility_reasons.size() > 0) {
    auto i = 0;
    for (const auto& reason : visibility->visibility_reasons) {
      util::ProtoToArgsParser::Key key;
      key.key = "visibility_reason[" + std::to_string(i) + ']';
      key.flat_key = "visibility_reason";
      writer.AddString(key, reason);
      i++;
    }
  }
  TryAddBlockingLayerArgs(visibility->occluding_layers, "occluded_by", writer);
  TryAddBlockingLayerArgs(visibility->partially_occluding_layers,
                          "partially_occluded_by", writer);
  TryAddBlockingLayerArgs(visibility->covering_layers, "covered_by", writer);
}

tables::SurfaceFlingerLayerTable::Id SurfaceFlingerLayersParser::InsertLayerRow(
    protozero::ConstBytes blob,
    tables::SurfaceFlingerLayersSnapshotTable::Id snapshot_id,
    const std::optional<VisibilityProperties>& visibility,
    const std::unordered_map<int, protozero::ConstBytes>& layers_by_id,
    const SfRectComputation::SurfaceFlingerRects& rects) {
  tables::SurfaceFlingerLayerTable::Row layer;
  layer.snapshot_id = snapshot_id;
  layer.base64_proto_id = context_->storage->mutable_string_pool()
                              ->InternString(base::StringView(
                                  base::Base64Encode(blob.data, blob.size)))
                              .raw_id();
  protos::pbzero::LayerProto::Decoder layer_decoder(blob);
  layer.layer_id = layer_decoder.id();
  if (layer_decoder.has_name()) {
    layer.layer_name = context_->storage->mutable_string_pool()->InternString(
        base::StringView(layer_decoder.name()));
  }
  if (layer_decoder.has_parent()) {
    layer.parent = layer_decoder.parent();
  }
  if (layer_decoder.has_corner_radius()) {
    layer.corner_radius = static_cast<double>(layer_decoder.corner_radius());
  }
  if (layer_decoder.has_hwc_composition_type()) {
    layer.hwc_composition_type = layer_decoder.hwc_composition_type();
  }
  if (layer_decoder.has_z_order_relative_of()) {
    layer.z_order_relative_of = layer_decoder.z_order_relative_of();
    if (layers_by_id.find(layer_decoder.z_order_relative_of()) ==
        layers_by_id.end()) {
      layer.is_missing_z_parent = true;
    }
  }
  layer.is_hidden_by_policy = SfLayer::IsHiddenByPolicy(layer_decoder);
  layer.is_visible = visibility.has_value() ? visibility->is_visible : false;
  layer.layer_rect_id = rects.layer_rect;
  layer.input_rect_id = rects.input_rect;
  return context_->storage->mutable_surfaceflinger_layer_table()
      ->Insert(layer)
      .id;
}

void SurfaceFlingerLayersParser::TryAddBlockingLayerArgs(
    const std::unordered_set<int>& blocking_layers,
    const std::string key_prefix,
    ArgsParser& writer) {
  if (blocking_layers.size() == 0) {
    return;
  }
  auto i = 0;
  for (auto blocking_layer : blocking_layers) {
    util::ProtoToArgsParser::Key key;
    key.key = key_prefix + "[" + std::to_string(i) + ']';
    key.flat_key = key_prefix;
    writer.AddInteger(key, blocking_layer);
    i++;
  }
}

void SurfaceFlingerLayersParser::ParseDisplay(
    const protos::pbzero::DisplayProto::Decoder& display_decoder,
    tables::SurfaceFlingerLayersSnapshotTable::Id snapshot_id,
    int index,
    std::unordered_map<uint32_t, WinscopeRect>& displays_by_layer_stack) {
  tables::SurfaceFlingerDisplayTable::Row display;
  display.snapshot_id = snapshot_id;
  display.is_virtual =
      display_decoder.has_is_virtual() ? display_decoder.is_virtual() : false;

  if (display_decoder.has_name()) {
    display.display_name =
        context_->storage->mutable_string_pool()->InternString(
            display_decoder.name());
  }

  if (display_decoder.has_layer_stack()) {
    display.is_on = display_decoder.layer_stack() != INVALID_LAYER_STACK;
  } else {
    display.is_on = false;
  }
  display.display_id = static_cast<int64_t>(display_decoder.id());

  const auto* rect_id =
      InsertDisplayRectRow(display_decoder, displays_by_layer_stack);

  display.trace_rect_id =
      InsertDisplayTraceRectRow(display_decoder, *rect_id, index);

  context_->storage->mutable_surfaceflinger_display_table()->Insert(display);
}

tables::WinscopeRectTable::Id* SurfaceFlingerLayersParser::InsertDisplayRectRow(
    const protos::pbzero::DisplayProto::Decoder& display_decoder,
    std::unordered_map<uint32_t, WinscopeRect>& displays_by_layer_stack) {
  WinscopeRect rect =
      SurfaceFlingerDisplay::MakeLayerStackSpaceRect(display_decoder);

  if (display_decoder.has_layer_stack()) {
    displays_by_layer_stack[display_decoder.layer_stack()] = rect;
  }

  if (rect.isEmpty()) {
    const auto& size = SurfaceFlingerDisplay::GetDisplaySize(display_decoder);
    rect = WinscopeRect::makeRect(0, 0, size.w, size.h);
  }

  auto* rect_tracker_ = WinscopeRectTracker::GetOrCreate(context_);
  return rect_tracker_->GetOrInsertRow(rect);
}

tables::WinscopeTraceRectTable::Id
SurfaceFlingerLayersParser::InsertDisplayTraceRectRow(
    const protos::pbzero::DisplayProto::Decoder& display_decoder,
    const tables::WinscopeRectTable::Id& rect_id,
    int index) {
  tables::WinscopeTraceRectTable::Row row;
  row.rect_id = rect_id;
  row.group_id = display_decoder.layer_stack();
  row.depth = static_cast<uint32_t>(index);
  row.is_spy = false;
  return context_->storage->mutable_winscope_trace_rect_table()->Insert(row).id;
}

}  // namespace trace_processor
}  // namespace perfetto
