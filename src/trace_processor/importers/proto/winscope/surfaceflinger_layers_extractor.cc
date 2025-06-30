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

#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_extractor.h"

#include <algorithm>
#include <functional>
#include <utility>
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_utils.h"

namespace perfetto::trace_processor::winscope::surfaceflinger_layers {

namespace {
enum ProcessingStage { VisitChildren, Add };

// When z-order is the same, we sort such that the layer with the layer id
// is drawn on top.
void SortByZThenLayerId(std::vector<ConstBytes>& layers) {
  std::sort(layers.begin(), layers.end(),
            [](const ConstBytes& a, const ConstBytes& b) {
              LayerDecoder layer_a(a);
              LayerDecoder layer_b(b);
              auto z_val_a = layer_a.z();
              auto z_val_b = layer_b.z();
              if (z_val_a != z_val_b) {
                return z_val_a > z_val_b;
              }
              return layer_a.id() >= layer_b.id();
            });
}

std::vector<ConstBytes> ExtractLayersByZOrder(
    std::vector<ConstBytes>& root_layers,
    std::unordered_map<int32_t, std::vector<ConstBytes>> children_by_z_parent) {
  SortByZThenLayerId(root_layers);

  std::vector<ConstBytes> layers_top_to_bottom;

  std::vector<std::pair<ConstBytes, ProcessingStage>> processing_queue;
  for (auto it = root_layers.rbegin(); it != root_layers.rend(); ++it) {
    processing_queue.emplace_back(*it, ProcessingStage::VisitChildren);
  }

  while (!processing_queue.empty()) {
    std::pair<ConstBytes, ProcessingStage> curr = processing_queue.back();
    processing_queue.pop_back();

    LayerDecoder curr_layer(curr.first);
    if (!curr_layer.has_id()) {
      continue;
    }

    std::vector<ConstBytes> curr_children;
    auto pos = children_by_z_parent.find(curr_layer.id());
    if (pos != children_by_z_parent.end()) {
      curr_children = pos->second;
      SortByZThenLayerId(curr_children);
    }

    int32_t current_z = curr_layer.z();

    if (curr.second == ProcessingStage::VisitChildren) {
      processing_queue.emplace_back(curr.first, ProcessingStage::Add);

      for (auto it = curr_children.rbegin(); it != curr_children.rend(); ++it) {
        LayerDecoder child_layer(*it);
        if (child_layer.z() >= current_z) {
          processing_queue.emplace_back(*it, ProcessingStage::VisitChildren);
        }
      }
    } else {
      layers_top_to_bottom.emplace_back(curr.first);

      for (auto it = curr_children.rbegin(); it != curr_children.rend(); ++it) {
        LayerDecoder child_layer(*it);
        if (child_layer.z() < current_z) {
          processing_queue.emplace_back(*it, ProcessingStage::VisitChildren);
        }
      }
    }
  }

  return layers_top_to_bottom;
}
}  // namespace

// Returns map of layer id to layer, so we can quickly retrieve a layer by its
// id during visibility computation.
std::unordered_map<int, ConstBytes> ExtractLayersById(
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

// Returns a vector of layers in top-to-bottom drawing order (z order), so
// we can determine occlusion states during visibility computation and depth
// in rect computation.
std::vector<ConstBytes> ExtractLayersTopToBottom(
    const LayersDecoder& layers_decoder) {
  std::vector<ConstBytes> root_layers;
  std::unordered_map<int32_t, std::vector<ConstBytes>> children_by_z_parent;

  for (auto it = layers_decoder.layers(); it; ++it) {
    auto layer = LayerDecoder(*it);
    if (layer::IsRootLayer(layer) && layer.z_order_relative_of() <= 0) {
      root_layers.emplace_back(*it);
      continue;
    }
    if (!layer.has_id()) {
      continue;
    }
    auto parent = layer.parent();
    auto z_parent = layer.z_order_relative_of();
    if (z_parent > 0) {
      children_by_z_parent[z_parent].emplace_back(*it);
    } else if (parent > 0) {
      children_by_z_parent[parent].emplace_back(*it);
    }
  }

  return ExtractLayersByZOrder(root_layers, children_by_z_parent);
}

}  // namespace perfetto::trace_processor::winscope::surfaceflinger_layers
