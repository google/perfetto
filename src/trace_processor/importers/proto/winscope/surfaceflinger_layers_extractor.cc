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
#include <vector>
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_utils.h"

namespace perfetto::trace_processor::winscope::surfaceflinger_layers {

namespace {
enum ProcessingStage { VisitChildren, Add };

std::vector<ConstBytes> ExtractRootLayers(const LayersDecoder& layers_decoder) {
  std::vector<ConstBytes> root_layers;
  for (auto it = layers_decoder.layers(); it; ++it) {
    auto root_layer = LayerDecoder(*it);
    if (layer::IsRootLayer(root_layer) &&
        root_layer.z_order_relative_of() <= 0) {
      root_layers.emplace_back(*it);
    }
  }
  return root_layers;
}

std::unordered_map<int32_t, std::vector<ConstBytes>> GroupChildrenByZParent(
    const LayersDecoder& layers_decoder) {
  std::unordered_map<int32_t, std::vector<ConstBytes>> children;
  for (auto it = layers_decoder.layers(); it; ++it) {
    LayerDecoder layer(*it);
    if (!layer.has_id()) {
      continue;
    }
    auto parent = layer.parent();
    auto z_parent = layer.z_order_relative_of();
    if (z_parent > 0) {
      children[z_parent].push_back(*it);
    } else if (parent > 0) {
      children[parent].push_back(*it);
    }
  }
  return children;
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

std::vector<ConstBytes> ExtractLayersTopToBottom(
    const LayersDecoder& layers_decoder) {
  auto root_layers = ExtractRootLayers(layers_decoder);
  SortByZThenLayerId(root_layers);

  auto children_by_z_parent = GroupChildrenByZParent(layers_decoder);

  std::vector<std::pair<ConstBytes, ProcessingStage>> processing_layers;
  for (auto it = root_layers.rbegin(); it != root_layers.rend(); ++it) {
    processing_layers.emplace_back(*it, ProcessingStage::VisitChildren);
  }

  std::vector<ConstBytes> layers_top_to_bottom;

  while (!processing_layers.empty()) {
    std::pair<ConstBytes, ProcessingStage> curr = processing_layers.back();
    processing_layers.pop_back();

    LayerDecoder current_layer(curr.first);
    if (!current_layer.has_id()) {
      continue;
    }

    auto pos = children_by_z_parent.find(current_layer.id());
    std::vector<ConstBytes> curr_children;
    if (pos != children_by_z_parent.end()) {
      curr_children = pos->second;
      SortByZThenLayerId(curr_children);
    }

    int32_t current_z = current_layer.z();

    if (curr.second == ProcessingStage::VisitChildren) {
      processing_layers.emplace_back(curr.first, ProcessingStage::Add);

      for (auto it = curr_children.rbegin(); it != curr_children.rend(); ++it) {
        LayerDecoder child_layer(*it);
        if (child_layer.z() >= current_z) {
          processing_layers.emplace_back(*it, ProcessingStage::VisitChildren);
        }
      }
    } else {
      layers_top_to_bottom.emplace_back(curr.first);

      for (auto it = curr_children.rbegin(); it != curr_children.rend(); ++it) {
        LayerDecoder child_layer(*it);
        if (child_layer.z() < current_z) {
          processing_layers.emplace_back(*it, ProcessingStage::VisitChildren);
        }
      }
    }
  }

  return layers_top_to_bottom;
}

}  // namespace perfetto::trace_processor::winscope::surfaceflinger_layers
