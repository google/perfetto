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

#include "src/trace_processor/importers/proto/winscope/viewcapture_visibility_computation.h"

#include <unordered_map>
#include <vector>

#include "src/trace_processor/importers/proto/winscope/viewcapture_test_utils.h"
#include "src/trace_processor/importers/proto/winscope/viewcapture_views_extractor.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope::viewcapture::test {

namespace {

std::unordered_map<int32_t, bool> ComputeVisibility(
    const std::string& snapshot) {
  protos::pbzero::ViewCapture::Decoder snapshot_decoder(snapshot);
  const std::vector<ViewDecoder> views_top_to_bottom =
      ExtractViewsTopToBottom(snapshot_decoder);
  return VisibilityComputation(views_top_to_bottom).Compute();
}

}  // namespace

TEST(ViewCaptureVisibilityComputation, RootNodeVisible) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddView(View().SetVisibility(0).SetParentId(-1))
                            .Build();

  auto result = ComputeVisibility(snapshot);
  ASSERT_TRUE(result.at(0));
}

TEST(ViewCaptureVisibilityComputation, ChildNodeVisible) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddView(View().SetVisibility(0).SetParentId(-1))
                            .AddView(View().SetVisibility(0).SetParentId(0))
                            .Build();

  auto result = ComputeVisibility(snapshot);
  ASSERT_TRUE(result.at(0));
  ASSERT_TRUE(result.at(1));
}

TEST(ViewCaptureVisibilityComputation, RootNodeNotVisible) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddView(View().SetVisibility(4).SetParentId(-1))
                            .Build();

  auto result = ComputeVisibility(snapshot);
  ASSERT_FALSE(result.at(0));
}

TEST(ViewCaptureVisibilityComputation, ChildNodeNotVisibleDueToParent) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddView(View().SetVisibility(4).SetParentId(-1))
                            .AddView(View().SetVisibility(0).SetParentId(0))
                            .Build();

  auto result = ComputeVisibility(snapshot);
  ASSERT_FALSE(result.at(0));
  ASSERT_FALSE(result.at(1));
}

TEST(ViewCaptureVisibilityComputation, ChildNodeNotVisibleButParentVisible) {
  const auto snapshot = SnapshotProtoBuilder()
                            .AddView(View().SetVisibility(0).SetParentId(-1))
                            .AddView(View().SetVisibility(4).SetParentId(0))
                            .Build();

  auto result = ComputeVisibility(snapshot);
  ASSERT_TRUE(result.at(0));
  ASSERT_FALSE(result.at(1));
}
}  // namespace perfetto::trace_processor::winscope::viewcapture::test
