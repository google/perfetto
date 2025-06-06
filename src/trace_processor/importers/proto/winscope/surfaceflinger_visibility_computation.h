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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_VISIBILITY_COMPUTATION_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_VISIBILITY_COMPUTATION_H_

#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layer_utils.h"
#include "src/trace_processor/importers/proto/winscope/winscope_rect.h"

namespace perfetto {
namespace trace_processor {

class SfVisibilityComputation {
 public:
  struct VisibilityProperties {
    bool is_visible;
    std::vector<std::string> visibility_reasons;
    std::unordered_set<int> occluding_layers;
    std::unordered_set<int> partially_occluding_layers;
    std::unordered_set<int> covering_layers;
  };

  SfVisibilityComputation* setSnapshot(
      const protos::pbzero::LayersSnapshotProto::Decoder& snapshot_decoder) {
    snapshot_decoder_ = &snapshot_decoder;
    return this;
  }

  SfVisibilityComputation* setLayersTopToBottom(
      const std::vector<protozero::ConstBytes>& layers_top_to_bottom) {
    layers_top_to_bottom_ = &layers_top_to_bottom;
    return this;
  }

  SfVisibilityComputation* setLayersById(
      const std::unordered_map<int, protozero::ConstBytes>* layers_by_id) {
    layers_by_id_ = layers_by_id;
    return this;
  }

  std::unordered_map<int, VisibilityProperties> Compute();

 private:
  static constexpr int LAYER_FLAG_OPAQUE = 0x02;

  const protos::pbzero::LayersSnapshotProto::Decoder* snapshot_decoder_;
  const std::vector<protozero::ConstBytes>* layers_top_to_bottom_;
  const std::unordered_map<int, protozero::ConstBytes>* layers_by_id_;
  std::vector<protozero::ConstBytes> opaque_layers = {};
  std::vector<protozero::ConstBytes> translucent_layers = {};

  VisibilityProperties isLayerVisible(protozero::ConstBytes layer_blob,
                                      bool excludes_composition_state,
                                      WinscopeRect* crop);

  bool isLayerVisibleInIsolation(const LayerDecoder& layer,
                                 bool excludes_composition_state);

  void updateVisibilityReasons(const LayerDecoder& layer,
                               bool excludes_composition_state,
                               VisibilityProperties& res);
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_VISIBILITY_COMPUTATION_H_
