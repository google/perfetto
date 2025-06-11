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

#include "src/trace_processor/importers/proto/winscope/surfaceflinger_visibility_computation.h"
#include <optional>
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/graphics/rect.pbzero.h"
#include "protos/perfetto/trace/android/surfaceflinger_common.pbzero.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layer_utils.h"

namespace perfetto {
namespace trace_processor {

using LayerDecoder = protos::pbzero::LayerProto::Decoder;
using ColorDecoder = protos::pbzero::ColorProto::Decoder;
using VisibilityProperties = SfVisibilityComputation::VisibilityProperties;

static bool isHiddenByParent(
    const LayerDecoder& layer,
    const std::unordered_map<int, protozero::ConstBytes>* layers_by_id) {
  if (SfLayer::IsRootLayer(layer)) {
    return false;
  }
  const auto parent_layer =
      LayerDecoder(layers_by_id->find(layer.parent())->second);
  return (SfLayer::IsHiddenByPolicy(parent_layer) ||
          isHiddenByParent(parent_layer, layers_by_id));
}

static bool isActiveBufferEmpty(const LayerDecoder& layer) {
  if (!layer.has_active_buffer())
    return true;
  auto buffer =
      protos::pbzero::ActiveBufferProto::Decoder(layer.active_buffer());
  return ((buffer.has_format() ? buffer.format() : 0) <= 0 &&
          (buffer.has_height() ? buffer.height() : 0) <= 0 &&
          (buffer.has_stride() ? buffer.stride() : 0) <= 0 &&
          (buffer.has_width() ? buffer.width() : 0) <= 0);
}

static bool hasEffects(const LayerDecoder& layer) {
  if (layer.has_shadow_radius() && layer.shadow_radius() > 0) {
    return true;
  }
  if (!layer.has_color()) {
    return false;
  }
  ColorDecoder color(layer.color());
  auto has_invalid_alpha = (color.has_a() ? color.a() : 0) <= 0;
  if (has_invalid_alpha) {
    return false;
  }
  auto has_invalid_rgb = (color.has_r() ? color.r() : 0) < 0 ||
                         (color.has_g() ? color.g() : 0) < 0 ||
                         (color.has_b() ? color.b() : 0) < 0;
  return !has_invalid_rgb;
}

static bool hasZeroAlpha(const LayerDecoder& layer) {
  if (!layer.has_color()) {
    return true;
  }
  ColorDecoder color(layer.color());
  if (!color.has_a()) {
    return true;
  }
  auto alpha = color.a();
  return alpha <= 0 && alpha > -1;
}

static bool hasEmptyVisibleRegion(const LayerDecoder& layer) {
  if (!layer.has_visible_region()) {
    return true;
  }
  const auto region =
      protos::pbzero::RegionProto::Decoder(layer.visible_region());
  if (region.has_rect()) {
    for (auto it = region.rect(); it; ++it) {
      protos::pbzero::RectProto::Decoder rect(*it);
      auto winscope_rect = WinscopeRect::makeRect(rect);
      if (!winscope_rect.isEmpty()) {
        return false;
      }
    }
  }
  return true;
}

static bool hasVisibleRegion(const LayerDecoder& layer,
                             bool excludes_composition_state) {
  if (excludes_composition_state) {
    // Doesn't include state sent during composition like visible region and
    // composition type, so we fallback on the bounds as the visible region
    return layer.has_bounds() && !SfLayer::GetBounds(layer).isEmpty();
  }
  return !hasEmptyVisibleRegion(layer);
}

static bool layerContains(LayerDecoder& layer,
                          LayerDecoder& other,
                          WinscopeRect* crop) {
  auto transform_type_layer = 0;
  if (layer.has_transform()) {
    protos::pbzero::TransformProto::Decoder transform(layer.transform());
    transform_type_layer = transform.has_type() ? transform.type() : 0;
  }

  auto transform_type_other = 0;
  if (layer.has_transform()) {
    protos::pbzero::TransformProto::Decoder transform(layer.transform());
    transform_type_other = transform.has_type() ? transform.type() : 0;
  }
  if (SfTransform::IsInvalidRotation(transform_type_layer) ||
      SfTransform::IsInvalidRotation(transform_type_other)) {
    return false;
  }
  const auto& layer_bounds = SfLayer::GetCroppedScreenBounds(layer, crop);
  const auto& other_bounds = SfLayer::GetCroppedScreenBounds(other, crop);
  return layer_bounds && other_bounds &&
         layer_bounds->containsRect(*other_bounds);
}

static bool layerOverlaps(LayerDecoder& layer,
                          LayerDecoder& other,
                          WinscopeRect* crop) {
  const auto& layer_bounds = SfLayer::GetCroppedScreenBounds(layer, crop);
  const auto& other_bounds = SfLayer::GetCroppedScreenBounds(other, crop);

  return layer_bounds && other_bounds &&
         layer_bounds->intersectsRect(*other_bounds);
}

static bool isOpaque(const LayerDecoder& layer) {
  if (!layer.has_color()) {
    return false;
  }
  ColorDecoder color(layer.color());
  const auto alpha = color.has_a() ? color.a() : 0;
  if (alpha < 1) {
    return false;
  }
  return layer.has_is_opaque() && layer.is_opaque();
}

static bool isColorEmpty(const LayerDecoder& layer) {
  if (!layer.has_color()) {
    return true;
  }
  ColorDecoder color(layer.color());
  return (color.a() <= 0 && color.a() > -1) || color.r() < 0 || color.g() < 0 ||
         color.b() < 0;
}

static WinscopeRect GetDisplayCropForLayer(
    LayerDecoder& layer,
    const protos::pbzero::LayersSnapshotProto::Decoder* snapshot_decoder) {
  WinscopeRect display_crop = WinscopeRect::makeRect(0, 0, 0, 0);
  if (!layer.has_layer_stack()) {
    return display_crop;
  }
  auto layer_stack = layer.layer_stack();
  for (auto it = snapshot_decoder->displays(); it; ++it) {
    protos::pbzero::DisplayProto::Decoder display(*it);
    if (!display.has_layer_stack() || display.layer_stack() != layer_stack) {
      continue;
    }
    if (!display.has_layer_stack_space_rect()) {
      continue;
    }
    display_crop = SurfaceFlingerDisplay::MakeLayerStackSpaceRect(display);
  }
  return display_crop;
}

std::unordered_map<int, VisibilityProperties>
SfVisibilityComputation::Compute() {
  std::unordered_map<int, VisibilityProperties> computed_visibility;
  auto excludes_composition_state =
      snapshot_decoder_->has_excludes_composition_state()
          ? snapshot_decoder_->excludes_composition_state()
          : true;
  for (auto it = layers_top_to_bottom_->begin();
       it != layers_top_to_bottom_->end(); it++) {
    LayerDecoder layer(*it);
    if (!layer.has_id()) {
      continue;
    }
    auto crop = GetDisplayCropForLayer(layer, snapshot_decoder_);
    const auto& res = isLayerVisible(*it, excludes_composition_state, &crop);

    computed_visibility[layer.id()] = res;
  }
  return computed_visibility;
}

VisibilityProperties SfVisibilityComputation::isLayerVisible(
    protozero::ConstBytes layer_blob,
    bool excludes_composition_state,
    WinscopeRect* crop) {
  LayerDecoder layer(layer_blob);

  VisibilityProperties res;
  res.is_visible = isLayerVisibleInIsolation(layer, excludes_composition_state);

  if (res.is_visible) {
    for (auto it = opaque_layers.begin(); it != opaque_layers.end(); it++) {
      auto opaque_layer = LayerDecoder(*it);
      if (opaque_layer.has_layer_stack() != layer.has_layer_stack()) {
        continue;
      }
      if (opaque_layer.has_layer_stack() && layer.has_layer_stack() &&
          opaque_layer.layer_stack() != layer.layer_stack()) {
        continue;
      }
      if (!layerContains(opaque_layer, layer, crop)) {
        continue;
      }

      auto corner_radius_layer =
          layer.has_corner_radius() ? layer.corner_radius() : 0;
      auto corner_radius_opaque_layer =
          opaque_layer.has_corner_radius() ? opaque_layer.corner_radius() : 0;
      if (corner_radius_opaque_layer <= corner_radius_layer) {
        res.is_visible = false;
        res.occluding_layers.insert(opaque_layer.id());
      }
    }

    for (auto it = opaque_layers.begin(); it != opaque_layers.end(); it++) {
      auto opaque_layer = LayerDecoder(*it);
      if (opaque_layer.has_layer_stack() != layer.has_layer_stack()) {
        continue;
      }
      if (opaque_layer.has_layer_stack() && layer.has_layer_stack() &&
          opaque_layer.layer_stack() != layer.layer_stack()) {
        continue;
      }
      if (!layerOverlaps(opaque_layer, layer, crop)) {
        continue;
      }

      if (res.occluding_layers.find(opaque_layer.id()) ==
          res.occluding_layers.end()) {
        res.partially_occluding_layers.insert(opaque_layer.id());
      }
    }

    for (auto it = translucent_layers.begin(); it != translucent_layers.end();
         it++) {
      auto translucent_layer = LayerDecoder(*it);
      if (translucent_layer.has_layer_stack() != layer.has_layer_stack()) {
        continue;
      }
      if (translucent_layer.has_layer_stack() && layer.has_layer_stack() &&
          translucent_layer.layer_stack() != layer.layer_stack()) {
        continue;
      }
      if (layerOverlaps(translucent_layer, layer, crop)) {
        res.covering_layers.insert(translucent_layer.id());
      }
    }

    if (isOpaque(layer)) {
      opaque_layers.push_back(layer_blob);
    } else {
      translucent_layers.push_back(layer_blob);
    }
  }

  if (!res.is_visible) {
    updateVisibilityReasons(layer, excludes_composition_state, res);
  }
  return res;
}

bool SfVisibilityComputation::isLayerVisibleInIsolation(
    const LayerDecoder& layer,
    bool excludes_composition_state) {
  if (isHiddenByParent(layer, layers_by_id_) ||
      SfLayer::IsHiddenByPolicy(layer)) {
    return false;
  }

  if (!layer.has_color()) {
    return false;
  }

  ColorDecoder color(layer.color());
  if (!color.has_a() || color.a() <= 0) {
    return false;
  }
  if (isActiveBufferEmpty(layer) && !hasEffects(layer)) {
    return false;
  }
  return hasVisibleRegion(layer, excludes_composition_state);
}

void SfVisibilityComputation::updateVisibilityReasons(
    const LayerDecoder& layer,
    bool excludes_composition_state,
    VisibilityProperties& res) {
  if (SfLayer::IsHiddenByPolicy(layer)) {
    res.visibility_reasons.push_back("flag is hidden");
  }

  if (isHiddenByParent(layer, layers_by_id_)) {
    res.visibility_reasons.push_back(
        std::string("hidden by parent " + std::to_string(layer.parent())));
  }

  if (isActiveBufferEmpty(layer)) {
    res.visibility_reasons.push_back("buffer is empty");
  }

  if (hasZeroAlpha(layer)) {
    res.visibility_reasons.push_back("alpha is 0");
  }

  if (!layer.has_bounds() || SfLayer::GetBounds(layer).isEmpty()) {
    res.visibility_reasons.push_back("bounds is 0x0");

    if (!layer.has_color() || (layer.has_color() && isColorEmpty(layer))) {
      res.visibility_reasons.push_back("crop is 0x0");
    }
  }

  if (!SfLayer::GetTransformMatrix(layer).isValid()) {
    res.visibility_reasons.push_back("transform is invalid");
  }

  if (isActiveBufferEmpty(layer) && !hasEffects(layer) &&
      !(layer.has_background_blur_radius() &&
        layer.background_blur_radius() > 0)) {
    res.visibility_reasons.push_back(
        "does not have color fill, shadow or blur");
  }

  if (layer.has_visible_region() && hasEmptyVisibleRegion(layer)) {
    res.visibility_reasons.push_back(
        "visible region calculated by Composition Engine is empty");
  }

  if (!layer.has_visible_region() && !excludes_composition_state) {
    res.visibility_reasons.push_back("null visible region");
  }

  if (res.occluding_layers.size() > 0) {
    res.visibility_reasons.push_back("occluded");
  }
}
}  // namespace trace_processor
}  // namespace perfetto
