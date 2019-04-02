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

#include "src/traced/probes/packages_list/packages_list_data_source.h"

#include <gtest/gtest.h>

namespace perfetto {
namespace {

TEST(PackagesListDataSourceTest, ParseLineNonProfileNonDebug) {
  char kLine[] =
      "com.test.app 1234 0 /data/user/0/com.test.app "
      "default:targetSdkVersion=12452 1234,5678 0 1111\n";
  Package pkg;
  ASSERT_TRUE(ReadPackagesListLine(kLine, &pkg));
  EXPECT_EQ(pkg.name, "com.test.app");
  EXPECT_EQ(pkg.debuggable, false);
  EXPECT_EQ(pkg.profileable_from_shell, false);
  EXPECT_EQ(pkg.version_code, 1111);
}

TEST(PackagesListDataSourceTest, ParseLineProfileNonDebug) {
  char kLine[] =
      "com.test.app 1234 0 /data/user/0/com.test.app "
      "default:targetSdkVersion=12452 1234,5678 1 1111\n";
  Package pkg;
  ASSERT_TRUE(ReadPackagesListLine(kLine, &pkg));
  EXPECT_EQ(pkg.name, "com.test.app");
  EXPECT_EQ(pkg.debuggable, false);
  EXPECT_EQ(pkg.profileable_from_shell, true);
  EXPECT_EQ(pkg.version_code, 1111);
}

TEST(PackagesListDataSourceTest, ParseLineNonProfileDebug) {
  char kLine[] =
      "com.test.app 1234 1 /data/user/0/com.test.app "
      "default:targetSdkVersion=12452 1234,5678 0 1111\n";
  Package pkg;
  ASSERT_TRUE(ReadPackagesListLine(kLine, &pkg));
  EXPECT_EQ(pkg.name, "com.test.app");
  EXPECT_EQ(pkg.debuggable, true);
  EXPECT_EQ(pkg.profileable_from_shell, false);
  EXPECT_EQ(pkg.version_code, 1111);
}

TEST(PackagesListDataSourceTest, ParseLineProfileDebug) {
  char kLine[] =
      "com.test.app 1234 1 /data/user/0/com.test.app "
      "default:targetSdkVersion=12452 1234,5678 1 1111\n";
  Package pkg;
  ASSERT_TRUE(ReadPackagesListLine(kLine, &pkg));
  EXPECT_EQ(pkg.name, "com.test.app");
  EXPECT_EQ(pkg.debuggable, true);
  EXPECT_EQ(pkg.profileable_from_shell, true);
  EXPECT_EQ(pkg.version_code, 1111);
}

}  // namespace
}  // namespace perfetto
