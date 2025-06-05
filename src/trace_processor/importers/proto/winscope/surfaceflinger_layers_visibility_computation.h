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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYERS_VISIBILITY_COMPUTATION_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYERS_VISIBILITY_COMPUTATION_H_

#include <unordered_map>
#include <unordered_set>
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_utils.h"
#include "src/trace_processor/importers/proto/winscope/winscope_geometry.h"

namespace perfetto::trace_processor::winscope::surfaceflinger_layers {

struct VisibilityProperties {
  bool is_visible;
  std::vector<StringPool::Id> visibility_reasons;
  std::vector<int> occluding_layers;
  std::vector<int> partially_occluding_layers;
  std::vector<int> covering_layers;
};

class VisibilityComputation {
 public:
  VisibilityComputation() = default;

  VisibilityComputation& SetSnapshot(
      const protos::pbzero::LayersSnapshotProto::Decoder& snapshot_decoder) {
    snapshot_decoder_ = &snapshot_decoder;
    return *this;
  }

  VisibilityComputation& SetLayersTopToBottom(
      const std::vector<protozero::ConstBytes>& layers_top_to_bottom) {
    layers_top_to_bottom_ = &layers_top_to_bottom;
    return *this;
  }

  VisibilityComputation& SetLayersById(
      const std::unordered_map<int, protozero::ConstBytes>* layers_by_id) {
    layers_by_id_ = layers_by_id;
    return *this;
  }

  VisibilityComputation& SetStringPool(StringPool* pool) {
    pool_ = pool;
    return *this;
  }

  std::unordered_map<int, VisibilityProperties> Compute();

 private:
  const protos::pbzero::LayersSnapshotProto::Decoder* snapshot_decoder_;
  const std::vector<protozero::ConstBytes>* layers_top_to_bottom_;
  const std::unordered_map<int, protozero::ConstBytes>* layers_by_id_;
  StringPool* pool_;
  std::vector<protozero::ConstBytes> opaque_layers = {};
  std::vector<protozero::ConstBytes> translucent_layers = {};

  VisibilityProperties IsLayerVisible(protozero::ConstBytes layer_blob,
                                      bool excludes_composition_state,
                                      geometry::Rect* crop);

  bool IsLayerVisibleInIsolation(
      const protos::pbzero::LayerProto::Decoder& layer,
      bool excludes_composition_state);

  std::vector<StringPool::Id> GetVisibilityReasons(
      const protos::pbzero::LayerProto::Decoder& layer,
      bool excludes_composition_state,
      std::vector<int>& occluding_layers);
};

}  // namespace perfetto::trace_processor::winscope::surfaceflinger_layers

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYERS_VISIBILITY_COMPUTATION_H_
