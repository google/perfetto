/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_utils.h"

#include <optional>

#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_test_utils.h"
#include "src/trace_processor/importers/proto/winscope/winscope_geometry_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope::surfaceflinger_layers::test {

namespace {
const Color test_color{0, 0, 0, 1};

protos::pbzero::LayerProto::Decoder ConvertToLayerProto(std::string& snapshot) {
  protos::pbzero::LayersSnapshotProto::Decoder snapshot_decoder(snapshot);
  protos::pbzero::LayersProto::Decoder layers_decoder(
      snapshot_decoder.layers());
  auto it = layers_decoder.layers();
  protos::pbzero::LayerProto::Decoder layer_decoder(*it);
  return layer_decoder;
}
}  // namespace

TEST(SfLayersUtils, IsRootLayerNoParent) {
  auto layer = Layer();
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  ASSERT_TRUE(layer::IsRootLayer(layer_proto));
}

TEST(SfLayersUtils, IsRootLayerInvalidParent) {
  auto layer = Layer().SetParent(-1);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  ASSERT_TRUE(layer::IsRootLayer(layer_proto));
}

TEST(SfLayersUtils, IsRootLayerValidParent) {
  auto layer = Layer().SetParent(1);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  ASSERT_FALSE(layer::IsRootLayer(layer_proto));
}

TEST(SfLayersUtils, IsHiddenByPolicyFlagSet) {
  auto layer = Layer().SetFlags(0x01);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  ASSERT_TRUE(layer::IsHiddenByPolicy(layer_proto));
}

TEST(SfLayersUtils, IsHiddenByPolicyOffscreenLayer) {
  auto layer = Layer().SetId(0x7ffffffd);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  ASSERT_TRUE(layer::IsHiddenByPolicy(layer_proto));
}

TEST(SfLayersUtils, IsHiddenByPolicyFalse) {
  auto layer = Layer();
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  ASSERT_FALSE(layer::IsHiddenByPolicy(layer_proto));
}

TEST(SfLayersUtils, GetBounds) {
  auto rect = geometry::Rect(1, 2, 3, 4);
  auto layer = Layer().SetBounds(rect).SetColor(test_color);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  const auto& extracted_bounds = layer::GetBounds(layer_proto);
  ASSERT_TRUE(extracted_bounds == rect);
}

TEST(SfLayersUtils, GetCroppedScreenBoundsNoCrop) {
  auto rect = geometry::Rect(1, 2, 3, 4);
  auto layer = Layer().SetScreenBounds(rect).SetColor(test_color);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  const auto& extracted_bounds =
      layer::GetCroppedScreenBounds(layer_proto, std::nullopt);
  ASSERT_TRUE(extracted_bounds.value() == rect);
}

TEST(SfLayersUtils, GetCroppedScreenBoundsValidCrop) {
  auto rect = geometry::Rect(1, 2, 3, 4);
  auto crop = geometry::Rect(0, 0, 2, 3);
  auto layer = Layer().SetScreenBounds(rect).SetColor(test_color);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  const auto& extracted_bounds =
      layer::GetCroppedScreenBounds(layer_proto, crop);
  ASSERT_TRUE(extracted_bounds == geometry::Rect(1, 2, 2, 3));
}

TEST(SfLayersUtils, GetCornerRadiiFromCornerRadiiField) {
  auto radii = geometry::CornerRadii{0.1, 0.2, 0.3, 0.4};
  auto layer = Layer().SetColor(test_color).SetCornerRadii(radii);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  auto extracted_radii = layer::GetCornerRadii(layer_proto);
  ASSERT_TRUE(geometry::test::IsCornerRadiiEqual(extracted_radii, radii));
}

TEST(SfLayersUtils, GetCornerRadiiFromCornerRadiusField) {
  auto layer = Layer().SetColor(test_color).SetCornerRadius(0.25);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  auto extracted_radii = layer::GetCornerRadii(layer_proto);
  ASSERT_TRUE(geometry::test::IsCornerRadiiEqual(extracted_radii,
                                                 {0.25, 0.25, 0.25, 0.25}));
}

TEST(SfLayersUtils, GetCornerRadiiFromEffectiveRadiiField) {
  auto radii = geometry::CornerRadii{0.1, 0.2, 0.3, 0.4};
  auto layer = Layer().SetColor(test_color).SetEffectiveRadii(radii);
  auto snapshot = SnapshotProtoBuilder().AddLayer(layer).Build();
  const auto& layer_proto = ConvertToLayerProto(snapshot);
  auto extracted_radii = layer::GetCornerRadii(layer_proto);
  ASSERT_TRUE(geometry::test::IsCornerRadiiEqual(extracted_radii, radii));
}
}  // namespace perfetto::trace_processor::winscope::surfaceflinger_layers::test
