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

#include "perfetto/ext/base/hash.h"

#include "perfetto/ext/base/string_view.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(HashTest, StringView) {
  base::StringView a = "abc";
  base::StringView b = "def";
  EXPECT_NE(Hasher::Combine(a), Hasher::Combine(b));
}

}  // namespace
}  // namespace base
}  // namespace perfetto
