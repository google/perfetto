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

#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_visibility_computation.h"
#include <optional>
#include "protos/perfetto/trace/android/graphics/rect.pbzero.h"
#include "protos/perfetto/trace/android/surfaceflinger_common.pbzero.h"

namespace perfetto::trace_processor::winscope::surfaceflinger_layers {

namespace {
using LayerDecoder = protos::pbzero::LayerProto::Decoder;
using ColorDecoder = protos::pbzero::ColorProto::Decoder;
using VisibilityProperties = VisibilityComputation::VisibilityProperties;

inline bool IsHiddenByParent(
    const LayerDecoder& layer,
    const std::unordered_map<int, protozero::ConstBytes>* layers_by_id) {
  if (layer::IsRootLayer(layer)) {
    return false;
  }
  const auto parent_layer =
      LayerDecoder(layers_by_id->find(layer.parent())->second);
  return (layer::IsHiddenByPolicy(parent_layer) ||
          IsHiddenByParent(parent_layer, layers_by_id));
}

inline bool IsActiveBufferEmpty(const LayerDecoder& layer) {
  if (!layer.has_active_buffer())
    return true;
  auto buffer =
      protos::pbzero::ActiveBufferProto::Decoder(layer.active_buffer());
  return buffer.format() <= 0 && buffer.height() <= 0 && buffer.stride() <= 0 &&
         buffer.width() <= 0;
}

inline bool HasEffects(const LayerDecoder& layer) {
  if (layer.shadow_radius() > 0) {
    return true;
  }
  if (!layer.has_color()) {
    return false;
  }
  ColorDecoder color(layer.color());
  auto has_invalid_alpha = color.a() <= 0;
  if (has_invalid_alpha) {
    return false;
  }
  auto has_invalid_rgb = color.r() < 0 || color.g() < 0 || color.b() < 0;
  return !has_invalid_rgb;
}

inline bool HasZeroAlpha(const LayerDecoder& layer) {
  if (!layer.has_color()) {
    return true;
  }
  ColorDecoder color(layer.color());
  auto alpha = color.a();
  return alpha <= 0 && alpha > -1;
}

inline bool HasEmptyVisibleRegion(const LayerDecoder& layer) {
  if (!layer.has_visible_region()) {
    return true;
  }
  const auto region =
      protos::pbzero::RegionProto::Decoder(layer.visible_region());
  if (region.has_rect()) {
    for (auto it = region.rect(); it; ++it) {
      protos::pbzero::RectProto::Decoder rect(*it);
      auto winscope_rect = geometry::Rect(rect);
      if (!winscope_rect.IsEmpty()) {
        return false;
      }
    }
  }
  return true;
}

inline bool HasVisibleRegion(const LayerDecoder& layer,
                             bool excludes_composition_state) {
  if (excludes_composition_state) {
    // Doesn't include state sent during composition like visible region and
    // composition type, so we fallback on the bounds as the visible region
    return layer.has_bounds() && !layer::GetBounds(layer).IsEmpty();
  }
  return !HasEmptyVisibleRegion(layer);
}

inline bool LayerContains(LayerDecoder& layer,
                          LayerDecoder& other,
                          geometry::Rect* crop) {
  auto transform_type_layer = 0;
  if (layer.has_transform()) {
    protos::pbzero::TransformProto::Decoder transform(layer.transform());
    transform_type_layer = transform.type();
  }

  auto transform_type_other = 0;
  if (layer.has_transform()) {
    protos::pbzero::TransformProto::Decoder transform(layer.transform());
    transform_type_other = transform.type();
  }
  if (transform::IsInvalidRotation(transform_type_layer) ||
      transform::IsInvalidRotation(transform_type_other)) {
    return false;
  }
  auto layer_bounds = layer::GetCroppedScreenBounds(layer, crop);
  auto other_bounds = layer::GetCroppedScreenBounds(other, crop);
  return layer_bounds && other_bounds &&
         layer_bounds->ContainsRect(*other_bounds);
}

inline bool LayerOverlaps(LayerDecoder& layer,
                          LayerDecoder& other,
                          geometry::Rect* crop) {
  auto layer_bounds = layer::GetCroppedScreenBounds(layer, crop);
  auto other_bounds = layer::GetCroppedScreenBounds(other, crop);

  return layer_bounds && other_bounds &&
         layer_bounds->IntersectsRect(*other_bounds);
}

inline bool IsOpaque(const LayerDecoder& layer) {
  if (!layer.has_color()) {
    return false;
  }
  ColorDecoder color(layer.color());
  if (color.a() < 1) {
    return false;
  }
  return layer.has_is_opaque() && layer.is_opaque();
}

inline bool IsColorEmpty(const LayerDecoder& layer) {
  if (!layer.has_color()) {
    return true;
  }
  ColorDecoder color(layer.color());
  return (color.a() <= 0 && color.a() > -1) || color.r() < 0 || color.g() < 0 ||
         color.b() < 0;
}

inline geometry::Rect GetDisplayCropForLayer(
    LayerDecoder& layer,
    const protos::pbzero::LayersSnapshotProto::Decoder* snapshot_decoder) {
  geometry::Rect display_crop = geometry::Rect();
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
    display_crop = display::MakeLayerStackSpaceRect(display);
  }
  return display_crop;
}
}  // namespace

std::unordered_map<int, VisibilityProperties> VisibilityComputation::Compute() {
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
    const auto& res = IsLayerVisible(*it, excludes_composition_state, &crop);

    computed_visibility[layer.id()] = res;
  }
  return computed_visibility;
}

VisibilityProperties VisibilityComputation::IsLayerVisible(
    protozero::ConstBytes layer_blob,
    bool excludes_composition_state,
    geometry::Rect* crop) {
  LayerDecoder layer(layer_blob);

  VisibilityProperties res;
  res.is_visible = IsLayerVisibleInIsolation(layer, excludes_composition_state);

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
      if (!LayerContains(opaque_layer, layer, crop)) {
        continue;
      }

      auto corner_radius_layer = layer.corner_radius();
      auto corner_radius_opaque_layer = opaque_layer.corner_radius();
      if (corner_radius_opaque_layer <= corner_radius_layer) {
        res.is_visible = false;
        res.occluding_layers.push_back(opaque_layer.id());
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
      if (!LayerOverlaps(opaque_layer, layer, crop)) {
        continue;
      }

      if (std::find(res.occluding_layers.begin(), res.occluding_layers.end(),
                    opaque_layer.id()) == res.occluding_layers.end()) {
        res.partially_occluding_layers.push_back(opaque_layer.id());
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
      if (LayerOverlaps(translucent_layer, layer, crop)) {
        res.covering_layers.push_back(translucent_layer.id());
      }
    }

    if (IsOpaque(layer)) {
      opaque_layers.push_back(layer_blob);
    } else {
      translucent_layers.push_back(layer_blob);
    }
  }

  if (!res.is_visible) {
    res.visibility_reasons = GetVisibilityReasons(
        layer, excludes_composition_state, res.occluding_layers);
  }

  return res;
}

bool VisibilityComputation::IsLayerVisibleInIsolation(
    const LayerDecoder& layer,
    bool excludes_composition_state) {
  if (IsHiddenByParent(layer, layers_by_id_) ||
      layer::IsHiddenByPolicy(layer)) {
    return false;
  }
  if (!layer.has_color()) {
    return false;
  }
  ColorDecoder color(layer.color());
  if (color.a() <= 0) {
    return false;
  }
  if (IsActiveBufferEmpty(layer) && !HasEffects(layer)) {
    return false;
  }
  return HasVisibleRegion(layer, excludes_composition_state);
}

std::vector<StringPool::Id> VisibilityComputation::GetVisibilityReasons(
    const LayerDecoder& layer,
    bool excludes_composition_state,
    std::vector<int>& occluding_layers) {
  std::vector<StringPool::Id> reasons;

  if (layer::IsHiddenByPolicy(layer)) {
    reasons.push_back(pool_->InternString(base::StringView("flag is hidden")));
  }

  if (IsHiddenByParent(layer, layers_by_id_)) {
    reasons.push_back(pool_->InternString(base::StringView(
        "hidden by parent " + std::to_string(layer.parent()))));
  }

  if (IsActiveBufferEmpty(layer)) {
    reasons.push_back(pool_->InternString(base::StringView("buffer is empty")));
  }

  if (HasZeroAlpha(layer)) {
    reasons.push_back(pool_->InternString(base::StringView("alpha is 0")));
  }

  if (!layer.has_bounds() || layer::GetBounds(layer).IsEmpty()) {
    reasons.push_back(pool_->InternString(base::StringView("bounds is 0x0")));

    if (!layer.has_color() || (layer.has_color() && IsColorEmpty(layer))) {
      reasons.push_back(pool_->InternString(base::StringView("crop is 0x0")));
    }
  }

  if (!layer::GetTransformMatrix(layer).IsValid()) {
    reasons.push_back(
        pool_->InternString(base::StringView("transform is invalid")));
  }

  if (IsActiveBufferEmpty(layer) && !HasEffects(layer) &&
      !(layer.background_blur_radius() > 0)) {
    reasons.push_back(pool_->InternString(
        base::StringView("does not have color fill, shadow or blur")));
  }

  if (layer.has_visible_region() && HasEmptyVisibleRegion(layer)) {
    reasons.push_back(pool_->InternString(base::StringView(
        "visible region calculated by Composition Engine is empty")));
  }

  if (!layer.has_visible_region() && !excludes_composition_state) {
    reasons.push_back(
        pool_->InternString(base::StringView("null visible region")));
  }

  if (occluding_layers.size() > 0) {
    reasons.push_back(pool_->InternString(base::StringView("occluded")));
  }

  return reasons;
}
}  // namespace perfetto::trace_processor::winscope::surfaceflinger_layers
