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
#include <string>
#include <utility>

#include "test/gtest_and_gmock.h"

namespace perfetto::base {

TEST(StatusTest, HugeError) {
  std::string x(4096, 'x');
  base::Status status = base::ErrStatus("%s", x.c_str());
  ASSERT_EQ(status.message(), x);
}

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

TEST(StatusTest, OkHasEmptyMessage) {
  Status status = OkStatus();
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.message(), "");
}

TEST(StatusTest, CopyClonesError) {
  Status error = ErrStatus("bad");
  error.SetPayload("test.foo.com/bar", "1");
  Status copy = error;
  copy.SetPayload("test.foo.com/bar", "2");
  EXPECT_EQ(copy.message(), "bad");
  EXPECT_EQ(error.GetPayload("test.foo.com/bar"), "1");
  EXPECT_EQ(copy.GetPayload("test.foo.com/bar"), "2");

  copy = OkStatus();
  EXPECT_TRUE(copy.ok());
  copy = error;
  EXPECT_EQ(copy.message(), "bad");
}

// Using a moved-from status is a bug, so it must not pass for ok.
TEST(StatusTest, MovedFromIsAnError) {
  Status ok = OkStatus();
  Status error = ErrStatus("bad");
  Status ok_moved = std::move(ok);
  Status error_moved = std::move(error);
  EXPECT_TRUE(ok_moved.ok());
  EXPECT_EQ(error_moved.message(), "bad");
  EXPECT_FALSE(ok.ok());     // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(error.ok());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(error.message(), "Status accessed after move");

  // A moved-from status can be assigned again.
  error = ErrStatus("again");
  EXPECT_EQ(error.message(), "again");
}

}  // namespace perfetto::base
