/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/base/http/sha1.h"

#include <string>

#include "perfetto/ext/base/string_view.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using testing::ElementsAreArray;

TEST(SHA1Test, Hash) {
  EXPECT_THAT(SHA1Hash(""), ElementsAreArray<uint8_t>(
                                {0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b,
                                 0x0d, 0x32, 0x55, 0xbf, 0xef, 0x95, 0x60,
                                 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09}));

  EXPECT_THAT(SHA1Hash("abc"), ElementsAreArray<uint8_t>(
                                   {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81,
                                    0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50,
                                    0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d}));

  EXPECT_THAT(
      SHA1Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      ElementsAreArray<uint8_t>({0x84, 0x98, 0x3e, 0x44, 0x1c, 0x3b, 0xd2,
                                 0x6e, 0xba, 0xae, 0x4a, 0xa1, 0xf9, 0x51,
                                 0x29, 0xe5, 0xe5, 0x46, 0x70, 0xf1}));

  EXPECT_THAT(
      SHA1Hash(std::string(1000000, 'a')),
      ElementsAreArray<uint8_t>({0x34, 0xaa, 0x97, 0x3c, 0xd4, 0xc4, 0xda,
                                 0xa4, 0xf6, 0x1e, 0xeb, 0x2b, 0xdb, 0xad,
                                 0x27, 0x31, 0x65, 0x34, 0x01, 0x6f}));
}

}  // namespace
}  // namespace base
}  // namespace perfetto
