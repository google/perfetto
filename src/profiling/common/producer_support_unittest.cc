/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/profiling/common/producer_support.h"

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

TEST(CanProfileAndroidTest, NonUserSystemExtraGuardrails) {
  DataSourceConfig ds_config;
  ds_config.set_enable_extra_guardrails(true);
  EXPECT_TRUE(CanProfileAndroid(ds_config, 1, "userdebug", "/dev/null"));
}

TEST(CanProfileAndroidTest, NonUserNonProfileableApp) {
  DataSourceConfig ds_config;
  ds_config.set_enable_extra_guardrails(false);
  auto tmp = base::TempFile::Create();
  constexpr char content[] =
      "invalid.example.profileable 10001 0 "
      "/data/user/0/invalid.example.profileable default:targetSdkVersion=10000 "
      "none 0 1\n";
  base::WriteAll(tmp.fd(), content, sizeof(content));
  EXPECT_TRUE(CanProfileAndroid(ds_config, 10001, "userdebug", tmp.path()));
}

TEST(CanProfileAndroidTest, NonUserNonProfileableAppExtraGuardrails) {
  DataSourceConfig ds_config;
  ds_config.set_enable_extra_guardrails(true);
  auto tmp = base::TempFile::Create();
  constexpr char content[] =
      "invalid.example.profileable 10001 0 "
      "/data/user/0/invalid.example.profileable default:targetSdkVersion=10000 "
      "none 0 1\n";
  base::WriteAll(tmp.fd(), content, sizeof(content));
  EXPECT_TRUE(CanProfileAndroid(ds_config, 10001, "userdebug", tmp.path()));
}

TEST(CanProfileAndroidTest, UserProfileableApp) {
  DataSourceConfig ds_config;
  ds_config.set_enable_extra_guardrails(false);
  auto tmp = base::TempFile::Create();
  constexpr char content[] =
      "invalid.example.profileable 10001 0 "
      "/data/user/0/invalid.example.profileable default:targetSdkVersion=10000 "
      "none 1 1\n";
  base::WriteAll(tmp.fd(), content, sizeof(content));
  EXPECT_TRUE(CanProfileAndroid(ds_config, 10001, "user", tmp.path()));
}

TEST(CanProfileAndroidTest, UserProfileableAppExtraGuardrails) {
  DataSourceConfig ds_config;
  ds_config.set_enable_extra_guardrails(true);
  auto tmp = base::TempFile::Create();
  constexpr char content[] =
      "invalid.example.profileable 10001 0 "
      "/data/user/0/invalid.example.profileable default:targetSdkVersion=10000 "
      "none 1 1\n";
  base::WriteAll(tmp.fd(), content, sizeof(content));
  EXPECT_FALSE(CanProfileAndroid(ds_config, 10001, "user", tmp.path()));
}

TEST(CanProfileAndroidTest, UserProfileableAppMultiuser) {
  DataSourceConfig ds_config;
  ds_config.set_enable_extra_guardrails(false);
  auto tmp = base::TempFile::Create();
  constexpr char content[] =
      "invalid.example.profileable 10001 0 "
      "/data/user/0/invalid.example.profileable default:targetSdkVersion=10000 "
      "none 1 1\n";
  base::WriteAll(tmp.fd(), content, sizeof(content));
  EXPECT_TRUE(CanProfileAndroid(ds_config, 210001, "user", tmp.path()));
}

TEST(CanProfileAndroidTest, UserNonProfileableApp) {
  DataSourceConfig ds_config;
  ds_config.set_enable_extra_guardrails(false);
  auto tmp = base::TempFile::Create();
  constexpr char content[] =
      "invalid.example.profileable 10001 0 "
      "/data/user/0/invalid.example.profileable default:targetSdkVersion=10000 "
      "none 0 1\n";
  base::WriteAll(tmp.fd(), content, sizeof(content));
  EXPECT_FALSE(CanProfileAndroid(ds_config, 10001, "user", tmp.path()));
}

TEST(CanProfileAndroidTest, UserDebuggableApp) {
  DataSourceConfig ds_config;
  ds_config.set_enable_extra_guardrails(false);
  auto tmp = base::TempFile::Create();
  constexpr char content[] =
      "invalid.example.profileable 10001 1 "
      "/data/user/0/invalid.example.profileable default:targetSdkVersion=10000 "
      "none 0 1\n";
  base::WriteAll(tmp.fd(), content, sizeof(content));
  EXPECT_TRUE(CanProfileAndroid(ds_config, 10001, "user", tmp.path()));
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
