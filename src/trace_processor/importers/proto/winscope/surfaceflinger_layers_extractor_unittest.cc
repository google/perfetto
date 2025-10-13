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

#include <unordered_map>
#include <vector>

#include "src/trace_processor/importers/proto/winscope/surfaceflinger_layers_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope::surfaceflinger_layers::test {

namespace {

std::unordered_map<int32_t, LayerDecoder> ExtractById(
    const std::string& snapshot) {
  protos::pbzero::LayersSnapshotProto::Decoder snapshot_decoder(snapshot);
  protos::pbzero::LayersProto::Decoder layers_decoder(
      snapshot_decoder.layers());
  return ExtractLayersById(layers_decoder);
}

void CheckExtractionTopToBottom(const std::string& snapshot,
                                const std::vector<int32_t> expected) {
  protos::pbzero::LayersSnapshotProto::Decoder snapshot_decoder(snapshot);
  protos::pbzero::LayersProto::Decoder layers_decoder(
      snapshot_decoder.layers());
  const auto& result = ExtractLayersTopToBottom(layers_decoder);

  std::vector<int32_t> layer_ids;
  for (const auto& layer : result) {
    layer_ids.push_back(layer.id());
  }
  ASSERT_EQ(layer_ids, expected);
}

}  // namespace

TEST(SfLayersExtractLayersById, IgnoresDuplicateLayerIds) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(Layer().SetId(3).SetZ(1))
                            .AddLayer(Layer().SetId(3).SetZ(2))
                            .Build();
  auto result = ExtractById(snapshot);

  ASSERT_EQ(result.size(), static_cast<size_t>(1));
  const auto& layer = result.at(3);
  ASSERT_EQ(layer.id(), 3);
  ASSERT_EQ(layer.z(), 1);
}

TEST(SfLayersExtractLayersById, IgnoresMissingLayerIds) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(Layer())
                            .AddLayer(Layer().NullifyId())
                            .Build();
  auto result = ExtractById(snapshot);

  ASSERT_EQ(result.size(), static_cast<size_t>(1));
  ASSERT_EQ(result.at(1).id(), 1);
}

TEST(SfLayersExtractLayersTopToBottom, SortsByZ) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(Layer().SetZ(1))
                            .AddLayer(Layer().SetZ(2))
                            .AddLayer(Layer().SetZ(0))
                            .Build();
  CheckExtractionTopToBottom(snapshot, {2, 1, 3});
}

TEST(SfLayersExtractLayersTopToBottom, SortsByZRestrictedToHierarchyLevel) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(Layer().SetZ(0))
                            .AddLayer(Layer().SetZ(0))
                            .AddLayer(Layer().SetZ(2).SetParent(2))
                            .AddLayer(Layer().SetZ(1).SetParent(2))
                            .Build();
  CheckExtractionTopToBottom(snapshot, {3, 4, 2, 1});
}

TEST(SfLayersExtractLayersTopToBottom, HandlesRelativeLayers) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(Layer().SetZ(1))
                            .AddLayer(Layer().SetZ(1).SetParent(1))
                            .AddLayer(Layer().SetZ(1))
                            .AddLayer(Layer().SetZ(0).SetZOrderRelativeOf(1))
                            .Build();
  CheckExtractionTopToBottom(snapshot, {3, 2, 4, 1});
}

TEST(SfLayersExtractLayersTopToBottom, HandlesNegativeZValues) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(Layer().SetZ(1))
                            .AddLayer(Layer().SetZ(0).SetParent(1))
                            .AddLayer(Layer().SetZ(-5))
                            .Build();
  CheckExtractionTopToBottom(snapshot, {2, 1, 3});
}

TEST(SfLayersExtractLayersTopToBottom, LayerIdFallback) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(Layer().SetZ(2))
                            .AddLayer(Layer().SetZ(2))
                            .Build();
  CheckExtractionTopToBottom(snapshot, {2, 1});
}

TEST(SfLayersExtractLayersTopToBottom, LayerIdFallbackOnlyForSiblings) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddLayer(Layer().SetZ(2).SetParent(2))
                            .AddLayer(Layer().SetZ(2))
                            .Build();
  CheckExtractionTopToBottom(snapshot, {1, 2});
}
}  // namespace perfetto::trace_processor::winscope::surfaceflinger_layers::test
