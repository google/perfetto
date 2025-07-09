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
#include <unordered_map>
#include <vector>

#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_extractor.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope::surfaceflinger_layers::test {

namespace {
Layer MakeVisibleLayer() {
  return Layer()
      .SetColor(Color{0, 0, 0, 1})
      .SetActiveBuffer(ActiveBuffer{1, 1, 1, 1})
      .SetBounds(geometry::Rect(0, 0, 1080, 2400))
      .SetScreenBounds(geometry::Rect(0, 0, 1080, 2400))
      .AddVisibleRegionRect(geometry::Rect(0, 0, 1080, 2400));
}

std::unordered_map<int32_t, VisibilityProperties> ComputeVisibility(
    const std::string& snapshot,
    StringPool* pool) {
  protos::pbzero::LayersSnapshotProto::Decoder snapshot_decoder(snapshot);

  protos::pbzero::LayersProto::Decoder layers_decoder(
      snapshot_decoder.layers());

  const auto layers_top_to_bottom = ExtractLayersTopToBottom(layers_decoder);
  const auto layers_by_id = ExtractLayersById(
      protos::pbzero::LayersProto::Decoder(snapshot_decoder.layers()));

  return VisibilityComputation(snapshot_decoder, layers_top_to_bottom,
                               layers_by_id, pool)
      .Compute();
}

void CheckLayerVisible(
    const std::unordered_map<int32_t, VisibilityProperties>& result,
    int32_t id) {
  auto properties = result.at(id);

  ASSERT_TRUE(properties.is_visible);

  ASSERT_EQ(properties.covering_layers.size(), static_cast<size_t>(0));
  ASSERT_EQ(properties.partially_occluding_layers.size(),
            static_cast<size_t>(0));
  ASSERT_EQ(properties.occluding_layers.size(), static_cast<size_t>(0));
  ASSERT_EQ(properties.visibility_reasons.size(), static_cast<size_t>(0));
}

void CheckReasons(const VisibilityProperties& properties,
                  const std::vector<std::string> reasons,
                  StringPool& pool) {
  ASSERT_EQ(properties.visibility_reasons.size(), reasons.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(reasons.size()); i++) {
    std::string reason =
        pool.Get(properties.visibility_reasons[i]).ToStdString();
    std::string expected_reason = reasons[i];
    ASSERT_EQ(reason, expected_reason);
  }
}

void CheckLayerNotVisibleInIsolation(
    const std::unordered_map<int32_t, VisibilityProperties>& result,
    const std::vector<std::string> reasons,
    StringPool& pool) {
  const auto& properties = result.at(1);

  ASSERT_FALSE(properties.is_visible);
  ASSERT_EQ(properties.covering_layers.size(), static_cast<size_t>(0));
  ASSERT_EQ(properties.partially_occluding_layers.size(),
            static_cast<size_t>(0));
  ASSERT_EQ(properties.occluding_layers.size(), static_cast<size_t>(0));

  CheckReasons(properties, reasons, pool);
}

}  // namespace

TEST(SfVisibilityComputation, VisibleNonEmptyVisibleRegion) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(Layer()
                        .SetColor(Color{0, 0, 0, 1})
                        .SetActiveBuffer(ActiveBuffer{1, 1, 1, 1})
                        .SetScreenBounds(geometry::Rect(0, 0, 1080, 2400))
                        .AddVisibleRegionRect(geometry::Rect(0, 0, 1080, 2400)))
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerVisible(result, 1);
}

TEST(SfVisibilityComputation, VisibleValidBounds) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .SetExcludesCompositionState(true)
          .AddLayer(Layer()
                        .SetColor(Color{0, 0, 0, 1})
                        .SetActiveBuffer(ActiveBuffer{1, 1, 1, 1})
                        .SetBounds(geometry::Rect(0, 0, 1080, 2400)))
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerVisible(result, 1);
}

TEST(SfVisibilityComputation, NotVisibleEmptyBoundsAndCrop) {
  const auto snapshot = SnapshotProtoBuilder()
                            .SetExcludesCompositionState(true)
                            .AddLayer(MakeVisibleLayer()
                                          .SetColor(Color{-1, -1, -1, 1})
                                          .SetBounds(geometry::Rect()))
                            .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerNotVisibleInIsolation(result, {"bounds is 0x0", "crop is 0x0"},
                                  pool);
}

TEST(SfVisibilityComputation, NotVisibleHiddenByPolicy) {
  const auto snapshot =
      SnapshotProtoBuilder().AddLayer(MakeVisibleLayer().SetFlags(1)).Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerNotVisibleInIsolation(result, {"flag is hidden"}, pool);
}

TEST(SfVisibilityComputation, NotVisibleHiddenByParent) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(MakeVisibleLayer().SetParent(2).SetZOrderRelativeOf(3))
          .AddLayer(MakeVisibleLayer().SetFlags(1))  // parent hidden
          .AddLayer(MakeVisibleLayer())              // z parent not hidden
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerNotVisibleInIsolation(result, {"hidden by parent 2"}, pool);
}

TEST(SfVisibilityComputation, NotVisibleZeroAlpha) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(MakeVisibleLayer().SetColor(Color{0, 0, 0, 0}))
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerNotVisibleInIsolation(result, {"alpha is 0"}, pool);
}

TEST(SfVisibilityComputation, NotVisibleNullActiveBufferAndNoEffects) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(Layer()
                        .SetColor(Color{-1, 0, 0, 1})
                        .SetScreenBounds(geometry::Rect(0, 0, 1080, 2400))
                        .SetBounds(geometry::Rect(0, 0, 1080, 2400))
                        .AddVisibleRegionRect(geometry::Rect(0, 0, 1080, 2400)))
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerNotVisibleInIsolation(
      result, {"buffer is empty", "does not have color fill, shadow or blur"},
      pool);
}

TEST(SfVisibilityComputation, NotVisibleEmptyActiveBufferAndNoEffects) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(Layer()
                        .SetColor(Color{-1, 0, 0, 1})
                        .SetActiveBuffer(ActiveBuffer{0, 0, 0, 0})
                        .SetScreenBounds(geometry::Rect(0, 0, 1080, 2400))
                        .SetBounds(geometry::Rect(0, 0, 1080, 2400))
                        .AddVisibleRegionRect(geometry::Rect(0, 0, 1080, 2400)))
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerNotVisibleInIsolation(
      result, {"buffer is empty", "does not have color fill, shadow or blur"},
      pool);
}

TEST(SfVisibilityComputation, NotVisibleNullVisibleRegion) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(Layer()
                        .SetColor(Color{0, 0, 0, 1})
                        .SetActiveBuffer(ActiveBuffer{1, 1, 1, 1})
                        .SetBounds(geometry::Rect(0, 0, 1080, 2400))
                        .SetScreenBounds(geometry::Rect(0, 0, 1080, 2400)))
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerNotVisibleInIsolation(result, {"null visible region"}, pool);
}

TEST(SfVisibilityComputation, NotVisibleEmptyVisibleRegion) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(MakeVisibleLayer().InitializeVisibleRegion())
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerNotVisibleInIsolation(
      result, {"visible region calculated by Composition Engine is empty"},
      pool);
}

TEST(SfVisibilityComputation, NotVisibleOccluded) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(MakeVisibleLayer().SetIsOpaque(true))
                            .AddLayer(MakeVisibleLayer().SetIsOpaque(true))
                            .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);

  const auto& layer1_properties = result.at(1);
  ASSERT_FALSE(layer1_properties.is_visible);

  ASSERT_EQ(layer1_properties.covering_layers.size(), static_cast<size_t>(0));
  ASSERT_EQ(layer1_properties.partially_occluding_layers.size(),
            static_cast<size_t>(0));
  ASSERT_EQ(layer1_properties.occluding_layers, std::vector<int32_t>{2});

  CheckReasons(layer1_properties, {"occluded"}, pool);

  CheckLayerVisible(result, 2);
}

TEST(SfVisibilityComputation, VisibleAndCovered) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(MakeVisibleLayer().SetIsOpaque(true))
          .AddLayer(MakeVisibleLayer().SetIsOpaque(true).SetColor(
              Color{0, 0, 0, 0.5}))
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);

  const auto& layer1_properties = result.at(1);
  ASSERT_TRUE(layer1_properties.is_visible);

  ASSERT_EQ(layer1_properties.covering_layers, std::vector<int32_t>{2});
  ASSERT_EQ(layer1_properties.partially_occluding_layers.size(),
            static_cast<size_t>(0));
  ASSERT_EQ(layer1_properties.occluding_layers.size(), static_cast<size_t>(0));
  ASSERT_EQ(layer1_properties.visibility_reasons.size(),
            static_cast<size_t>(0));

  CheckLayerVisible(result, 2);
}

TEST(SfVisibilityComputation, VisibleNotOccludedDifferentLayerStack) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(MakeVisibleLayer().SetIsOpaque(true).SetLayerStack(1))
          .AddLayer(MakeVisibleLayer().SetIsOpaque(true))
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);
  CheckLayerVisible(result, 1);
  CheckLayerVisible(result, 2);
}

TEST(SfVisibilityComputation, VisiblePartiallyOccluded) {
  const auto snapshot =
      SnapshotProtoBuilder()
          .AddLayer(MakeVisibleLayer().SetIsOpaque(true))
          .AddLayer(MakeVisibleLayer().SetIsOpaque(true).SetScreenBounds(
              geometry::Rect(0, 0, 50, 50)))
          .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);

  const auto& layer1_properties = result.at(1);
  ASSERT_TRUE(layer1_properties.is_visible);

  ASSERT_EQ(layer1_properties.covering_layers.size(), static_cast<size_t>(0));
  ASSERT_EQ(layer1_properties.partially_occluding_layers,
            std::vector<int32_t>{2});
  ASSERT_EQ(layer1_properties.occluding_layers.size(), static_cast<size_t>(0));
  ASSERT_EQ(layer1_properties.visibility_reasons.size(),
            static_cast<size_t>(0));

  CheckLayerVisible(result, 2);
}

TEST(SfVisibilityComputation, VisibleNotOpaque) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(MakeVisibleLayer().SetIsOpaque(true))
                            .AddLayer(MakeVisibleLayer())
                            .Build();
  StringPool pool;

  auto result = ComputeVisibility(snapshot, &pool);

  const auto& layer1_properties = result.at(1);
  ASSERT_TRUE(layer1_properties.is_visible);

  ASSERT_EQ(layer1_properties.covering_layers, std::vector<int32_t>{2});
  ASSERT_EQ(layer1_properties.partially_occluding_layers.size(),
            static_cast<size_t>(0));
  ASSERT_EQ(layer1_properties.occluding_layers.size(), static_cast<size_t>(0));
  ASSERT_EQ(layer1_properties.visibility_reasons.size(),
            static_cast<size_t>(0));

  CheckLayerVisible(result, 2);
}
}  // namespace perfetto::trace_processor::winscope::surfaceflinger_layers::test
