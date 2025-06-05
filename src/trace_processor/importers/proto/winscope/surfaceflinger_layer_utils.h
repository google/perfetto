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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYER_UTILS_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYER_UTILS_H_

#include <optional>
#include "protos/perfetto/trace/android/graphics/rect.pbzero.h"
#include "protos/perfetto/trace/android/surfaceflinger_common.pbzero.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/importers/proto/winscope/winscope_rect.h"
#include "src/trace_processor/importers/proto/winscope/winscope_transform.h"

namespace perfetto {
namespace trace_processor {

using TransformDecoder = protos::pbzero::TransformProto::Decoder;

class SfTransform {
 public:
  static bool IsInvalidRotation(int flags) {
    return IsFlagSet(flags, TransformFlag::ROT_INVALID_VAL);
  }

  static bool IsRotated270(int flags) {
    return IsFlagSet(flags, TransformFlag::ROT_90_VAL |
                                TransformFlag::FLIP_V_VAL |
                                TransformFlag::FLIP_H_VAL);
  }

  static bool IsRotated180(int flags) {
    return IsFlagSet(flags,
                     TransformFlag::FLIP_V_VAL | TransformFlag::FLIP_H_VAL);
  }

  static bool IsRotated90(int flags) {
    return IsFlagSet(flags, TransformFlag::ROT_90_VAL);
  }

  static bool IsRotated(const TransformDecoder& transform) {
    auto transform_type = transform.has_type() ? transform.type() : 0;
    if (transform_type == 0) {
      return false;
    }
    auto invalidRotation = SfTransform::IsInvalidRotation(transform_type);
    if (invalidRotation) {
      return false;
    }
    auto rotated_270 = SfTransform::IsRotated270(transform_type);
    if (rotated_270) {
      return true;
    }
    return !SfTransform::IsRotated180(transform_type) &&
           SfTransform::IsRotated90(transform_type);
  }

  static bool IsSimpleTransform(int type) {
    return IsFlagClear(
        type, TransformFlag::ROT_INVALID_VAL | TransformFlag::SCALE_VAL);
  }

  static TransformMatrix GetTransformFromTypeAndPosition(int type,
                                                         double x,
                                                         double y) {
    // IDENTITY
    if (!type) {
      auto matrix = TransformMatrix{};
      matrix.tx = x;
      matrix.ty = y;
      return matrix;
    }

    // ROT_270 = ROT_90|FLIP_H|FLIP_V
    if (IsFlagSet(type, TransformFlag::ROT_90_VAL | TransformFlag::FLIP_V_VAL |
                            TransformFlag::FLIP_H_VAL)) {
      return TransformMatrix{0, -1, x, 1, 0, y};
    }

    // ROT_180 = FLIP_H|FLIP_V
    if (IsFlagSet(type,
                  TransformFlag::FLIP_V_VAL | TransformFlag::FLIP_H_VAL)) {
      return TransformMatrix{
          -1, 0, x, 0, -1, y,
      };
    }

    // ROT_90
    if (IsFlagSet(type, TransformFlag::ROT_90_VAL)) {
      return TransformMatrix{
          0, 1, x, -1, 0, y,
      };
    }
    auto matrix = TransformMatrix{};
    matrix.tx = x;
    matrix.ty = y;
    return matrix;
  }

 private:
  enum TransformFlag {
    EMPTY = 0x0,
    TRANSLATE_VAL = 0x0001,
    ROTATE_VAL = 0x0002,
    SCALE_VAL = 0x0004,
    FLIP_H_VAL = 0x0100,
    FLIP_V_VAL = 0x0200,
    ROT_90_VAL = 0x0400,
    ROT_INVALID_VAL = 0x8000,
  };

  static bool IsFlagSet(int flags, int bits) { return (flags & bits) == bits; }
  static bool IsFlagClear(int flags, int bits) { return (flags & bits) == 0; }
};

using LayerDecoder = protos::pbzero::LayerProto::Decoder;

class SfLayer {
 public:
  static bool IsRootLayer(const LayerDecoder& layer) {
    return !layer.has_parent() || layer.parent() == -1;
  }

  static bool IsHiddenByPolicy(const LayerDecoder& layer) {
    return ((layer.has_flags() && (layer.flags() & LAYER_FLAG_HIDDEN) != 0x0) ||
            (layer.has_id() && layer.id() == OFFSCREEN_LAYER_ROOT_ID));
  }

  static WinscopeRect GetBounds(const LayerDecoder& layer) {
    auto bounds = protos::pbzero::FloatRectProto::Decoder(layer.bounds());
    return WinscopeRect::makeRect(bounds);
  }

  static std::optional<WinscopeRect> GetCroppedScreenBounds(
      const LayerDecoder& layer,
      WinscopeRect* crop) {
    if (!layer.has_screen_bounds()) {
      return std::nullopt;
    }
    auto screen_bounds =
        protos::pbzero::FloatRectProto::Decoder(layer.screen_bounds());
    auto screen_bounds_rect = WinscopeRect::makeRect(screen_bounds);

    if (crop && !(crop->isEmpty())) {
      screen_bounds_rect = screen_bounds_rect.cropRect(*crop);
    }
    return screen_bounds_rect;
  }

  static TransformMatrix GetTransformMatrix(const LayerDecoder& layer_decoder) {
    TransformMatrix matrix;

    if (layer_decoder.has_position()) {
      protos::pbzero::PositionProto::Decoder position(layer_decoder.position());
      if (position.has_x())
        matrix.tx = static_cast<double>(position.x());
      if (position.has_y())
        matrix.ty = static_cast<double>(position.y());
    }

    if (layer_decoder.has_transform()) {
      TransformDecoder transform(layer_decoder.transform());

      auto type = transform.type();

      if (SfTransform::IsSimpleTransform(type)) {
        matrix = SfTransform::GetTransformFromTypeAndPosition(type, matrix.tx,
                                                              matrix.ty);
      } else {
        matrix.dsdx = static_cast<double>(transform.dsdx());
        matrix.dtdx = static_cast<double>(transform.dtdx());
        matrix.dsdy = static_cast<double>(transform.dtdy());
        matrix.dtdy = static_cast<double>(transform.dsdy());
      }
    }
    return matrix;
  }

 private:
  static constexpr int LAYER_FLAG_HIDDEN = 0x01;
  static constexpr int OFFSCREEN_LAYER_ROOT_ID = 0x7ffffffd;
};

using DisplayDecoder = protos::pbzero::DisplayProto::Decoder;

class SurfaceFlingerDisplay {
 public:
  static WinscopeRect MakeLayerStackSpaceRect(
      const DisplayDecoder& display_decoder) {
    protos::pbzero::RectProto::Decoder layer_stack_space_rect(
        display_decoder.layer_stack_space_rect());
    return WinscopeRect::makeRect(layer_stack_space_rect);
  }

  static TransformMatrix GetTransformMatrix(
      const DisplayDecoder& display_decoder) {
    TransformMatrix matrix;

    if (display_decoder.has_transform()) {
      TransformDecoder transform(display_decoder.transform());
      auto type = transform.type();

      if (SfTransform::IsSimpleTransform(type)) {
        matrix = SfTransform::GetTransformFromTypeAndPosition(type, 0, 0);
      } else {
        matrix.dsdx = static_cast<double>(transform.dsdx());
        matrix.dtdx = static_cast<double>(transform.dtdx());
        matrix.dsdy = static_cast<double>(transform.dtdy());
        matrix.dtdy = static_cast<double>(transform.dsdy());
      }
    }

    return matrix;
  }

  static Size GetDisplaySize(const DisplayDecoder& display_decoder) {
    if (!display_decoder.has_size()) {
      return Size{0, 0};
    }
    protos::pbzero::SizeProto::Decoder size_decoder(display_decoder.size());
    auto w = static_cast<double>(size_decoder.w());
    auto h = static_cast<double>(size_decoder.h());

    if (display_decoder.has_transform()) {
      TransformDecoder transform_decoder(display_decoder.transform());
      if (transform_decoder.has_type()) {
        auto transform_type = transform_decoder.type();
        if (SfTransform::IsRotated90(transform_type) ||
            SfTransform::IsRotated270(transform_type)) {
          return Size{h, w};
        }
      }
    }
    return Size{w, h};
  }
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYER_UTILS_H_
