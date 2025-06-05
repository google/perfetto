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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYERS_EXTRACTOR_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYERS_EXTRACTOR_H_

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_utils.h"

namespace perfetto::trace_processor::winscope::surfaceflinger_layers::
    extractor {

namespace {
using ConstBytes = protozero::ConstBytes;
using LayersDecoder = protos::pbzero::LayersProto::Decoder;
using LayerDecoder = protos::pbzero::LayerProto::Decoder;

enum ProcessingStage { Expand, Add };

void PopulateZChildren(
    std::unordered_map<int32_t, std::vector<ConstBytes>>* children,
    const LayersDecoder& layers_decoder) {
  for (auto it = layers_decoder.layers(); it; ++it) {
    LayerDecoder layer(*it);
    if (!layer.has_id()) {
      continue;
    }
    auto parent = layer.parent();
    auto z_parent = layer.z_order_relative_of();
    if (z_parent > 0) {
      (*children)[z_parent].push_back(*it);
    } else if (parent > 0) {
      (*children)[parent].push_back(*it);
    }
  }
}

void SortByZThenLayerId(std::vector<ConstBytes>& layers) {
  std::sort(layers.begin(), layers.end(),
            [](const ConstBytes& a, const ConstBytes& b) {
              LayerDecoder layer_a(a);
              LayerDecoder layer_b(b);
              auto z_val_a = layer_a.z();
              auto z_val_b = layer_b.z();
              if (z_val_a < z_val_b)
                return 0;
              if (z_val_a > z_val_b)
                return 1;
              return layer_a.id() < layer_b.id() ? 0 : 1;
            });
}
}  // namespace

inline std::unordered_map<int, ConstBytes> ExtractLayersById(
    const LayersDecoder& layers_decoder) {
  std::unordered_map<int, ConstBytes> layers_by_id;
  for (auto it = layers_decoder.layers(); it; ++it) {
    LayerDecoder layer(*it);
    if (!layer.has_id()) {
      continue;
    }
    layers_by_id[layer.id()] = *it;
  }
  return layers_by_id;
}

inline std::vector<ConstBytes> ExtractLayersTopToBottom(
    const LayersDecoder& layers_decoder) {
  std::vector<ConstBytes> root_layers;
  for (auto it = layers_decoder.layers(); it; ++it) {
    auto root_layer = LayerDecoder(*it);
    if (layer::IsRootLayer(root_layer) &&
        root_layer.z_order_relative_of() <= 0) {
      root_layers.emplace_back(*it);
    }
  }
  std::unordered_map<int32_t, std::vector<ConstBytes>> layer_z_children;
  PopulateZChildren(&layer_z_children, layers_decoder);

  SortByZThenLayerId(root_layers);

  std::vector<std::pair<ConstBytes, ProcessingStage>> processing_layers;
  for (auto it = root_layers.rbegin(); it != root_layers.rend(); ++it) {
    processing_layers.emplace_back(*it, ProcessingStage::Expand);
  }

  std::vector<ConstBytes> layers_top_to_bottom;
  while (!processing_layers.empty()) {
    std::pair<ConstBytes, ProcessingStage> curr = processing_layers.back();
    processing_layers.pop_back();

    LayerDecoder current_layer(curr.first);
    if (!current_layer.has_id()) {
      continue;
    }
    int32_t current_z = current_layer.z();

    auto children_pos = layer_z_children.find(current_layer.id());
    std::vector<ConstBytes> curr_children;
    if (children_pos != layer_z_children.end()) {
      curr_children = children_pos->second;
      SortByZThenLayerId(curr_children);
    }

    if (curr.second == ProcessingStage::Expand) {
      processing_layers.emplace_back(curr.first, ProcessingStage::Add);

      for (auto it = curr_children.rbegin(); it != curr_children.rend(); ++it) {
        LayerDecoder child_layer(*it);
        if (child_layer.z() >= current_z) {
          processing_layers.emplace_back(*it, ProcessingStage::Expand);
        }
      }
    } else {
      layers_top_to_bottom.emplace_back(curr.first);
      for (auto it = curr_children.rbegin(); it != curr_children.rend(); ++it) {
        LayerDecoder child_layer(*it);
        if (child_layer.z() < current_z) {
          processing_layers.emplace_back(*it, ProcessingStage::Expand);
        }
      }
    }
  }
  return layers_top_to_bottom;
}

}  // namespace
   // perfetto::trace_processor::winscope::surfaceflinger_layers::extractor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYERS_EXTRACTOR_H_
