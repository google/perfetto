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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_RECT_COMPUTATION_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_RECT_COMPUTATION_H_

#include <optional>
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/surfaceflinger_common.pbzero.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_visibility_computation.h"
#include "src/trace_processor/importers/proto/winscope/winscope_rect.h"
#include "src/trace_processor/importers/proto/winscope/winscope_transform.h"
#include "src/trace_processor/tables/winscope_tables_py.h"

namespace perfetto {
namespace trace_processor {

using VisibilityProperties = SfVisibilityComputation::VisibilityProperties;

class SfRectComputation {
 public:
  struct SurfaceFlingerRects {
    std::optional<tables::WinscopeTraceRectTable::Id> layer_rect = std::nullopt;
    std::optional<tables::WinscopeTraceRectTable::Id> input_rect = std::nullopt;
  };

  SfRectComputation* setSnapshot(
      const protos::pbzero::LayersSnapshotProto::Decoder& snapshot_decoder) {
    snapshot_decoder_ = &snapshot_decoder;
    return this;
  }

  SfRectComputation* setLayers(
      const std::vector<protozero::ConstBytes>& layers_top_to_bottom) {
    layers_top_to_bottom_ = &layers_top_to_bottom;
    return this;
  }

  SfRectComputation* setComputedVisibility(
      const std::unordered_map<int, VisibilityProperties>&
          computed_visibility) {
    computed_visibility_ = &computed_visibility;
    return this;
  }

  SfRectComputation* setDisplays(
      const std::unordered_map<uint32_t, WinscopeRect>&
          displays_by_layer_stack) {
    displays_by_layer_stack_ = &displays_by_layer_stack;
    return this;
  }

  SfRectComputation* setRectTracker(WinscopeRectTracker* rect_tracker) {
    rect_tracker_ = rect_tracker;
    return this;
  }

  SfRectComputation* setTransformTracker(
      WinscopeTransformTracker* transform_tracker) {
    transform_tracker_ = transform_tracker;
    return this;
  }

  const std::unordered_map<int32_t, SurfaceFlingerRects> Compute();

 private:
  // InputConfig constants defined in the platform:
  //   frameworks/native/libs/input/android/os/InputConfig.aidl
  enum InputConfig {
    NOT_TOUCHABLE = 1 << 3,
    IS_WALLPAPER = 1 << 6,
    SPY = 1 << 14,
  };

  const protos::pbzero::LayersSnapshotProto::Decoder* snapshot_decoder_;
  const std::vector<protozero::ConstBytes>* layers_top_to_bottom_;
  const std::unordered_map<int, VisibilityProperties>* computed_visibility_;
  const std::unordered_map<uint32_t, WinscopeRect>* displays_by_layer_stack_;
  WinscopeRectTracker* rect_tracker_;
  WinscopeTransformTracker* transform_tracker_;

  static constexpr WinscopeRect DEFAULT_INVALID_BOUNDS =
      WinscopeRect{-50000, -50000, 100000, 100000};

  std::vector<WinscopeRect> makeInvalidBoundsFromDisplays();

  std::unordered_map<uint32_t, TransformMatrix> extractDisplayTransforms();

  std::optional<tables::WinscopeTraceRectTable::Id> tryInsertBoundsRect(
      const protos::pbzero::LayerProto::Decoder& layer,
      const std::vector<WinscopeRect>& invalid_bounds,
      std::unordered_map<int32_t, int>& current_z_by_layer_stack);

  std::optional<tables::WinscopeTraceRectTable::Id> tryInsertInputRect(
      const protos::pbzero::LayerProto::Decoder& layer,
      const std::vector<WinscopeRect>& invalid_bounds,
      std::unordered_map<int32_t, int>& current_z_by_layer_stack,
      const std::unordered_map<uint32_t, TransformMatrix>& display_transforms);

  std::optional<Region> makeFillRegion(
      uint32_t input_config,
      const protos::pbzero::InputWindowInfoProto::Decoder& input_window_info,
      const std::optional<TransformMatrix>& display_transform,
      const TransformMatrix& inverse_layer_transform,
      const std::optional<WinscopeRect>& display);

  tables::WinscopeTraceRectTable::Id insertLayerTraceRectRow(
      const protos::pbzero::LayerProto::Decoder& layer_decoder,
      bool is_computed_visible,
      int absolute_z);

  tables::WinscopeTraceRectTable::Id insertInputTraceRectRow(
      WinscopeRect& frame_rect,
      const TransformMatrix& matrix,
      int absolute_z,
      int layer_stack,
      bool is_visible,
      bool is_spy);
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_RECT_COMPUTATION_H_
