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

#include "src/trace_processor/importers/proto/winscope/winscope_geometry.h"

#include "protos/perfetto/trace/android/graphics/rect.gen.h"
#include "src/trace_processor/importers/proto/winscope/winscope_geometry_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope::geometry::test {

namespace {
const Rect test_rect(1, 2, 10, 15);

void CheckRectEquality(Rect rect, Rect other) {
  ASSERT_EQ(rect.x, other.x);
  ASSERT_EQ(rect.y, other.y);
  ASSERT_EQ(rect.w, other.w);
  ASSERT_EQ(rect.h, other.h);
}
}  // namespace

TEST(WinscopeGeometryRect, BuildsFromLTRB) {
  ASSERT_EQ(test_rect.x, 1);
  ASSERT_EQ(test_rect.y, 2);
  ASSERT_EQ(test_rect.w, 9);
  ASSERT_EQ(test_rect.h, 13);
}

TEST(WinscopeGeometryRect, BuildsFromRectProto) {
  protos::gen::RectProto rect_proto;
  UpdateRect(&rect_proto, test_rect);
  auto blob = rect_proto.SerializeAsString();
  protos::pbzero::RectProto::Decoder rect_decoder(blob);
  Rect rect_from_proto(rect_decoder);
  CheckRectEquality(rect_from_proto, test_rect);
}

TEST(WinscopeGeometryRect, BuildsFromFloatRectProto) {
  protos::gen::FloatRectProto rect_proto;
  UpdateRect(&rect_proto, test_rect);
  auto blob = rect_proto.SerializeAsString();
  protos::pbzero::FloatRectProto::Decoder rect_decoder(blob);
  Rect rect_from_proto(rect_decoder);
  CheckRectEquality(rect_from_proto, test_rect);
}

TEST(WinscopeGeometryRectIsEmpty, ZeroRect) {
  Rect rect(0, 0, 0, 0);
  ASSERT_TRUE(rect.IsEmpty());
}

TEST(WinscopeGeometryRectIsEmpty, NegativeHW) {
  Rect rect(0, 0, -10, -10);
  ASSERT_TRUE(rect.IsEmpty());
}

TEST(WinscopeGeometryRectIsEmpty, ValidRect) {
  ASSERT_FALSE(test_rect.IsEmpty());
}

TEST(WinscopeGeometryRectIsEmpty, NegativeLT) {
  Rect rect(-1, -1, 0, 0);
  ASSERT_FALSE(rect.IsEmpty());
}

TEST(WinscopeGeometryRectCropRect, ReducesHeight) {
  Rect rect(0, 0, 2, 10);
  Rect crop(0, 0, 10, 5);
  auto cropped_rect = rect.CropRect(crop);
  Rect expected_rect(0, 0, 2, 5);
  CheckRectEquality(expected_rect, cropped_rect);
}

TEST(WinscopeGeometryRectCropRect, ReducesWidth) {
  Rect rect(0, 0, 10, 2);
  Rect crop(0, 0, 5, 10);
  auto cropped_rect = rect.CropRect(crop);
  Rect expected_rect(0, 0, 5, 2);
  CheckRectEquality(expected_rect, cropped_rect);
}

TEST(WinscopeGeometryRectCropRect, NoChangeForLargerCrop) {
  Rect rect(0, 0, 5, 5);
  Rect crop(0, 0, 10, 10);
  auto cropped_rect = rect.CropRect(crop);
  CheckRectEquality(cropped_rect, rect);
}

TEST(WinscopeGeometryRectContainsRect, SmallerBounds) {
  Rect other(1.5, 2.5, 9.5, 14.5);
  ASSERT_TRUE(test_rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, LargerBounds) {
  Rect rect(1.5, 2.5, 9.5, 14.5);
  ASSERT_FALSE(rect.ContainsRect(test_rect));
}

TEST(WinscopeGeometryRectContainsRect, ExactMatch) {
  ASSERT_TRUE(test_rect.ContainsRect(test_rect));
}

TEST(WinscopeGeometryRectContainsRect, MatchWithinThreshold) {
  Rect other(0.99994, 1.99994, 5, 5);
  ASSERT_TRUE(test_rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, ExactMatchLargerRadiusTl) {
  Rect other(1, 2, 10, 15);
  other.radii.tl = 1;
  ASSERT_TRUE(test_rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, SmallerBoundsSmallerRadiusTlContained) {
  Rect rect(1, 2, 10, 15);
  rect.radii.tl = 2;
  Rect other(2, 3, 9.5, 14.5);
  other.radii.tl = 1;
  ASSERT_TRUE(rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect,
     SmallerBoundsSmallerRadiusTlNotContained) {
  Rect rect(1, 2, 10, 15);
  rect.radii.tl = 2;
  Rect other(1.25, 2.25, 9.5, 14.5);
  other.radii.tl = 0.25;
  ASSERT_FALSE(rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, ExactMatchLargerRadiusTr) {
  Rect other(1, 2, 10, 15);
  other.radii.tr = 1;
  ASSERT_TRUE(test_rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, SmallerBoundsSmallerRadiusTrContained) {
  Rect rect(1, 2, 10, 15);
  rect.radii.tr = 2;
  Rect other(1, 3, 9, 15);
  other.radii.tr = 1;
  ASSERT_TRUE(rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect,
     SmallerBoundsSmallerRadiusTrNotContained) {
  Rect rect(1, 2, 10, 15);
  rect.radii.tr = 2;
  Rect other(1, 2.25, 10, 14.75);
  other.radii.tr = 0.25;
  ASSERT_FALSE(rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, ExactMatchLargerRadiusBl) {
  Rect other(1, 2, 10, 15);
  other.radii.bl = 1;
  ASSERT_TRUE(test_rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, SmallerBoundsSmallerRadiusBlContained) {
  Rect rect(1, 2, 10, 15);
  rect.radii.bl = 2;
  Rect other(2, 2, 10, 14);
  other.radii.bl = 1;
  ASSERT_TRUE(rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect,
     SmallerBoundsSmallerRadiusBlNotContained) {
  Rect rect(1, 2, 10, 15);
  rect.radii.bl = 2;
  Rect other(1.25, 2, 10, 14.75);
  other.radii.bl = 0.25;
  ASSERT_FALSE(rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, ExactMatchLargerRadiusBr) {
  Rect other(1, 2, 10, 15);
  other.radii.br = 1;
  ASSERT_TRUE(test_rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, SmallerBoundsSmallerRadiusBrContained) {
  Rect rect(1, 2, 10, 15);
  rect.radii.bl = 2;
  Rect other(1, 2, 9, 14);
  other.radii.bl = 1;
  ASSERT_TRUE(rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect,
     SmallerBoundsSmallerRadiusBrNotContained) {
  Rect rect(1, 2, 10, 15);
  rect.radii.br = 2;
  Rect other(1, 2, 9.75, 14.75);
  other.radii.br = 0.25;
  ASSERT_FALSE(rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectContainsRect, SmallerBoundsZeroRadii) {
  Rect rect(0, 1000, 1080, 2162);
  rect.radii.tl = 47;
  rect.radii.tr = 47;
  rect.radii.bl = 47;
  rect.radii.br = 47;
  Rect other(0, 1137, 1080, 1293);
  ASSERT_TRUE(rect.ContainsRect(other));
}

TEST(WinscopeGeometryRectIntersectsRect, ExactMatch) {
  ASSERT_TRUE(test_rect.IntersectsRect(test_rect));
}

TEST(WinscopeGeometryRectIntersectsRect, Overlap) {
  Rect rect(0, 0, 5, 5);
  Rect other(2, 2, 7, 7);
  ASSERT_TRUE(rect.IntersectsRect(other));
}

TEST(WinscopeGeometryRectIntersectsRect, NoOverlap) {
  Rect rect(0, 0, 5, 5);
  Rect other(5, 5, 10, 10);
  ASSERT_FALSE(rect.IntersectsRect(other));
}

TEST(WinscopeGeometryRectIsAlmostEqual, SameRects) {
  Rect other(1, 2, 10, 15);
  ASSERT_TRUE(test_rect.IsAlmostEqual(other));
}

TEST(WinscopeGeometryRectIsAlmostEqual, WithinThreshold) {
  Rect other(1, 2, 10, 15.005);
  ASSERT_TRUE(test_rect.IsAlmostEqual(other));
}

TEST(WinscopeGeometryRectIsAlmostEqual, OutsideThreshold) {
  Rect other(1, 2, 10, 15.011);
  ASSERT_FALSE(test_rect.IsAlmostEqual(other));
}

TEST(WinscopeGeometryRectIsAlmostEqual, DifferentRects) {
  Rect other(1, 2, 10, 16);
  ASSERT_FALSE(test_rect.IsAlmostEqual(other));
}
}  // namespace perfetto::trace_processor::winscope::geometry::test
