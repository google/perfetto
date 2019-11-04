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

#include "perfetto/ext/base/uuid.h"

#include "test/gtest_and_gmock.h"

#include "perfetto/ext/base/optional.h"

namespace perfetto {
namespace base {
namespace {

TEST(Uuid, TwoUuidsShouldBeDifferent) {
  Uuid a = Uuidv4();
  Uuid b = Uuidv4();
  EXPECT_NE(a, b);
}

TEST(Uuid, CanRoundTripUuid) {
  Uuid uuid = Uuidv4();
  EXPECT_EQ(StringToUuid(UuidToString(uuid)), uuid);
}

TEST(Uuid, BytesToUuid) {
  std::string empty = "";
  std::string too_short = "abc";
  std::string uuid = "abcdefghijklmnop";

  EXPECT_EQ(BytesToUuid(empty.data(), empty.size()), nullopt);
  EXPECT_EQ(BytesToUuid(uuid.data(), uuid.size()),
            Optional<Uuid>(StringToUuid(uuid)));
}

TEST(Uuid, UuidToPrettyString) {
  EXPECT_EQ(
      UuidToPrettyString(StringToUuid(
          "\x12\x3e\x45\x67\xe8\x9b\x12\xd3\xa4\x56\x42\x66\x55\x44\x33\x22")),
      "123e4567-e89b-12d3-a456-426655443322");
}

}  // namespace
}  // namespace base
}  // namespace perfetto
