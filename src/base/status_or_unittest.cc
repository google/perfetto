/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "perfetto/ext/base/status_or.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {

TEST(StatusOrTest, IntOk) {
  base::StatusOr<int> int_or = 1;
  ASSERT_TRUE(int_or.ok());
  ASSERT_TRUE(int_or.status().ok());
  ASSERT_EQ(int_or.value(), 1);
  ASSERT_EQ(*int_or, 1);
}

TEST(StatusOrTest, VecOk) {
  base::StatusOr<std::vector<int>> vec_or({0, 1, 100});
  ASSERT_TRUE(vec_or.ok());
  ASSERT_TRUE(vec_or.status().ok());

  ASSERT_EQ(vec_or.value()[0], 0);
  ASSERT_EQ(vec_or.value()[2], 100);

  ASSERT_EQ((*vec_or)[0], 0);
  ASSERT_EQ((*vec_or)[2], 100);

  ASSERT_EQ(vec_or->at(0), 0);
  ASSERT_EQ(vec_or->at(2), 100);
}

TEST(StatusOrTest, ErrStatus) {
  base::StatusOr<std::vector<int>> err(base::ErrStatus("Bad error"));
  ASSERT_FALSE(err.ok());
  ASSERT_FALSE(err.status().ok());
}

}  // namespace base
}  // namespace perfetto
