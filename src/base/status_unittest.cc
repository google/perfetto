/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "perfetto/base/status.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {

TEST(StatusTest, GetMissingPayload) {
  base::Status status = base::ErrStatus("Error");
  ASSERT_EQ(status.GetPayload("test.foo.com/bar"), std::nullopt);
}

TEST(StatusTest, SetThenGetPayload) {
  base::Status status = base::ErrStatus("Error");
  status.SetPayload("test.foo.com/bar", "payload_value");
  ASSERT_EQ(status.GetPayload("test.foo.com/bar"), "payload_value");
}

TEST(StatusTest, SetEraseGetPayload) {
  base::Status status = base::ErrStatus("Error");
  status.SetPayload("test.foo.com/bar", "payload_value");
  ASSERT_TRUE(status.ErasePayload("test.foo.com/bar"));
  ASSERT_EQ(status.GetPayload("test.foo.com/bar"), std::nullopt);
}

TEST(StatusTest, SetOverride) {
  base::Status status = base::ErrStatus("Error");
  status.SetPayload("test.foo.com/bar", "payload_value");
  status.SetPayload("test.foo.com/bar", "other_value");
  ASSERT_EQ(status.GetPayload("test.foo.com/bar"), "other_value");
}

TEST(StatusTest, SetGetOk) {
  base::Status status = base::OkStatus();
  status.SetPayload("test.foo.com/bar", "payload_value");
  ASSERT_EQ(status.GetPayload("test.foo.com/bar"), std::nullopt);
}

TEST(StatusTest, SetMultipleAndDuplicate) {
  base::Status status = base::ErrStatus("Error");
  status.SetPayload("test.foo.com/bar", "payload_value");
  status.SetPayload("test.foo.com/bar1", "1");
  status.SetPayload("test.foo.com/bar2", "2");
  status.SetPayload("test.foo.com/bar", "other_value");
  ASSERT_EQ(status.GetPayload("test.foo.com/bar"), "other_value");
  ASSERT_EQ(status.GetPayload("test.foo.com/bar1"), "1");
  ASSERT_EQ(status.GetPayload("test.foo.com/bar2"), "2");
}

}  // namespace base
}  // namespace perfetto
