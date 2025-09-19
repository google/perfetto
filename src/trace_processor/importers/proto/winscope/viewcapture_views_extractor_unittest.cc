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

#include "src/trace_processor/importers/proto/winscope/viewcapture_views_extractor.h"

#include <vector>

#include "src/trace_processor/importers/proto/winscope/viewcapture_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope::viewcapture::test {

namespace {

void CheckExtractionTopToBottom(const std::string& snapshot,
                                const std::vector<int32_t> expected) {
  protos::pbzero::ViewCapture::Decoder snapshot_decoder(snapshot);
  const auto& result = ExtractViewsTopToBottom(snapshot_decoder);

  std::vector<int32_t> layer_ids;
  for (const auto& layer : result) {
    layer_ids.push_back(layer.id());
  }
  ASSERT_EQ(layer_ids, expected);
}

}  // namespace

TEST(ViewCaptureExtractLayersTopToBottom, IdentifiesRootByParentId) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddView(View().SetId(1).SetParentId(0))
                            .AddView(View().SetId(3).SetParentId(1))
                            .AddView(View().SetId(2).SetParentId(0))
                            .AddView(View().SetId(0).SetParentId(-1))
                            .Build();
  CheckExtractionTopToBottom(snapshot, {0, 1, 3, 2});
}

TEST(ViewCaptureExtractLayersTopToBottom, RetrievesDfs) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddView(View().SetParentId(-1))
                            .AddView(View().SetParentId(0))
                            .AddView(View().SetParentId(0))
                            .AddView(View().SetParentId(1))
                            .AddView(View().SetParentId(2))
                            .AddView(View().SetParentId(2))
                            .AddView(View().SetParentId(5))
                            .Build();
  CheckExtractionTopToBottom(snapshot, {0, 1, 3, 2, 4, 5, 6});
}
}  // namespace perfetto::trace_processor::winscope::viewcapture::test
