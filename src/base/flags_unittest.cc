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

#include "perfetto/ext/base/flags.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::base::flags {
namespace {

TEST(FlagsTest, TestReadonlyFlag) {
  // Verify that this is accessible as a constexpr variable.
  static_assert(!base::flags::test_read_only_flag);

  // Verify that it's also the same as a runtime function.
  ASSERT_FALSE(base::flags::test_read_only_flag);
}

}  // namespace
}  // namespace perfetto::base::flags
