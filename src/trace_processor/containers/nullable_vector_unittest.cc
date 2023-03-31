/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/containers/nullable_vector.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(NullableVector, Append) {
  NullableVector<int64_t> sv;
  sv.Append(10);
  sv.Append(20);
  sv.Append(std::nullopt);
  sv.Append(40);

  ASSERT_FALSE(sv.IsDense());
  ASSERT_EQ(sv.size(), 4u);
  ASSERT_EQ(sv.Get(0), std::optional<int64_t>(10));
  ASSERT_EQ(sv.Get(1), std::optional<int64_t>(20));
  ASSERT_EQ(sv.Get(2), std::nullopt);
  ASSERT_EQ(sv.Get(3), std::optional<int64_t>(40));
}

TEST(NullableVector, Set) {
  NullableVector<int64_t> sv;
  sv.Append(10);
  sv.Append(20);
  sv.Append(std::nullopt);
  sv.Append(std::nullopt);
  sv.Append(40);

  sv.Set(0, 15);
  sv.Set(3, 30);

  ASSERT_EQ(*sv.Get(0), 15);
  ASSERT_EQ(*sv.Get(1), 20);
  ASSERT_EQ(sv.Get(2), std::nullopt);
  ASSERT_EQ(*sv.Get(3), 30);
  ASSERT_EQ(*sv.Get(4), 40);
}

TEST(NullableVector, SetNonNull) {
  NullableVector<int64_t> sv;
  sv.Append(1);
  sv.Append(2);
  sv.Append(3);
  sv.Append(4);

  sv.Set(1, 22);

  ASSERT_EQ(sv.Get(0), std::optional<int64_t>(1));
  ASSERT_EQ(sv.Get(1), std::optional<int64_t>(22));
  ASSERT_EQ(sv.Get(2), std::optional<int64_t>(3));
  ASSERT_EQ(sv.Get(3), std::optional<int64_t>(4));
}

TEST(NullableVector, Dense) {
  auto sv = NullableVector<int64_t>::Dense();

  sv.Append(0);
  sv.Append(std::nullopt);
  sv.Append(2);
  sv.Append(3);
  sv.Append(std::nullopt);

  ASSERT_TRUE(sv.IsDense());
  ASSERT_EQ(sv.Get(0), 0);
  ASSERT_EQ(sv.Get(1), std::nullopt);
  ASSERT_EQ(sv.Get(2), 2);
  ASSERT_EQ(sv.Get(3), 3);
  ASSERT_EQ(sv.Get(4), std::nullopt);

  sv.Set(1, 1);
  ASSERT_EQ(sv.Get(1), 1);
  ASSERT_EQ(sv.Get(2), 2);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
