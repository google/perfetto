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

#include <stdio.h>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

DataSourceConfig ShellInitiator() {
  DataSourceConfig ds_config;
  ds_config.set_session_initiator(
      DataSourceConfig::SESSION_INITIATOR_UNSPECIFIED);
  return ds_config;
}

DataSourceConfig TrustedInitiator() {
  DataSourceConfig ds_config;
  ds_config.set_session_initiator(
      DataSourceConfig::SESSION_INITIATOR_TRUSTED_SYSTEM);
  return ds_config;
}

// pkgName    - package name
// userId     - application-specific user id
// debugFlag  - 0 or 1 if the package is debuggable.
// dataPath   - path to package's data path
// seinfo     - seinfo label for the app (assigned at install time)
// gids       - supplementary gids this app launches with
// profileableFromShellFlag  - 0 or 1 if the package is profileable from shell.
// longVersionCode - integer version of the package.
// profileable - 0 or 1 if the package is profileable by the platform.
// packageInstaller - the package that installed this app, or @system, @product
//                     or @null.
std::string PackageListLine(unsigned long uid,
                            bool debuggable,
                            bool profileable_from_shell,
                            bool profileable,
                            const char* installer) {
  base::StackString<256> ss(
      "com.package.name %lu %d /data/user/0/com.package.name "
      "platform:privapp:targetSdkVersion=29 1065,3003 %d 500 %d %s\n",
      uid, debuggable, profileable_from_shell, profileable, installer);
  return ss.ToStdString();
}

TEST(CanProfileAndroidTest, DebuggableBuild) {
  unsigned pkg_uid = 10001;
  std::string content = PackageListLine(
      pkg_uid, /*debuggable=*/false, /*profileable_from_shell=*/false,
      /*profileable=*/false, /*installer=*/"@system");
  auto tmp = base::TempFile::Create();
  base::WriteAll(tmp.fd(), content.c_str(), content.size());

  // non-app UIDs can be profiled
  EXPECT_TRUE(CanProfileAndroid(ShellInitiator(), /*uid=*/200,
                                /*installed_by=*/{}, "userdebug", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), /*uid=*/200,
                                /*installed_by=*/{}, "userdebug", tmp.path()));

  // app UIDs can be profiled, regardless of manifest
  EXPECT_TRUE(CanProfileAndroid(ShellInitiator(), pkg_uid, /*installed_by=*/{},
                                "userdebug", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), pkg_uid,
                                /*installed_by=*/{}, "userdebug", tmp.path()));
}

TEST(CanProfileAndroidTest, DebuggableApp) {
  unsigned uid = 10001;
  std::string content = PackageListLine(
      uid, /*debuggable=*/true, /*profileable_from_shell=*/false,
      /*profileable=*/false, /*installer=*/"@system");
  auto tmp = base::TempFile::Create();
  base::WriteAll(tmp.fd(), content.c_str(), content.size());

  // Debuggable apps can always be profiled (without installer constraint)
  EXPECT_TRUE(CanProfileAndroid(ShellInitiator(), uid, /*installed_by=*/{},
                                "user", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), uid, /*installed_by=*/{},
                                "user", tmp.path()));
}

TEST(CanProfileAndroidTest, NonProfileableApp) {
  unsigned uid = 10002;
  std::string content = PackageListLine(
      uid, /*debuggable=*/false, /*profileable_from_shell=*/false,
      /*profileable=*/false, /*installer=*/"@system");
  auto tmp = base::TempFile::Create();
  base::WriteAll(tmp.fd(), content.c_str(), content.size());

  // Opted out packages cannot be profiled
  EXPECT_FALSE(CanProfileAndroid(ShellInitiator(), uid, /*installed_by=*/{},
                                 "user", tmp.path()));
  EXPECT_FALSE(CanProfileAndroid(TrustedInitiator(), uid, /*installed_by=*/{},
                                 "user", tmp.path()));
}

TEST(CanProfileAndroidTest, ProfileableApp) {
  unsigned uid = 10004;
  std::string content = PackageListLine(
      uid, /*debuggable=*/false, /*profileable_from_shell=*/false,
      /*profileable=*/true, /*installer=*/"@system");
  auto tmp = base::TempFile::Create();
  base::WriteAll(tmp.fd(), content.c_str(), content.size());

  // Only profileable by the platform
  EXPECT_FALSE(CanProfileAndroid(ShellInitiator(), uid, /*installed_by=*/{},
                                 "user", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), uid, /*installed_by=*/{},
                                "user", tmp.path()));
}

TEST(CanProfileAndroidTest, ProfileableFromShellApp) {
  unsigned uid = 10001;
  std::string content = PackageListLine(
      uid, /*debuggable=*/false, /*profileable_from_shell=*/true,
      /*profileable=*/true, /*installer=*/"@system");
  auto tmp = base::TempFile::Create();
  base::WriteAll(tmp.fd(), content.c_str(), content.size());

  EXPECT_TRUE(CanProfileAndroid(ShellInitiator(), uid, /*installed_by=*/{},
                                "user", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), uid, /*installed_by=*/{},
                                "user", tmp.path()));
}

// As ProfileableApp, but with a user profile offset
TEST(CanProfileAndroidTest, UserProfileUidOffset) {
  unsigned u0_uid = 10199;
  std::string content = PackageListLine(
      u0_uid, /*debuggable=*/false, /*profileable_from_shell=*/false,
      /*profileable=*/true, /*installer=*/"@system");
  auto tmp = base::TempFile::Create();
  base::WriteAll(tmp.fd(), content.c_str(), content.size());

  // Only profileable by the platform
  EXPECT_FALSE(CanProfileAndroid(ShellInitiator(), u0_uid, /*installed_by=*/{},
                                 "user", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), u0_uid, /*installed_by=*/{},
                                "user", tmp.path()));
  unsigned u10_uid = 1010199;
  EXPECT_FALSE(CanProfileAndroid(ShellInitiator(), u10_uid, /*installed_by=*/{},
                                 "user", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), u10_uid,
                                /*installed_by=*/{}, "user", tmp.path()));
}

// As ProfileableFromShellApp, but with installer constraints
TEST(CanProfileAndroidTest, InstallerPackageConstraint) {
  unsigned uid_installed_by_system = 10001;
  unsigned uid_installed_by_store = 10003;
  std::string content =
      PackageListLine(uid_installed_by_system, /*debuggable=*/false,
                      /*profileable_from_shell=*/true,
                      /*profileable=*/true, /*installer=*/"@system");
  content += PackageListLine(  //
      uid_installed_by_store, /*debuggable=*/false,
      /*profileable_from_shell=*/true,
      /*profileable=*/true, /*installer=*/"com.installer.package");
  auto tmp = base::TempFile::Create();
  base::WriteAll(tmp.fd(), content.c_str(), content.size());

  // Can profile if installer in the list (and other checks pass)
  // @system installer:
  EXPECT_TRUE(CanProfileAndroid(ShellInitiator(), uid_installed_by_system,
                                /*installed_by=*/{"@product", "@system"},
                                "user", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), uid_installed_by_system,
                                /*installed_by=*/{"@product", "@system"},
                                "user", tmp.path()));
  EXPECT_FALSE(CanProfileAndroid(ShellInitiator(), uid_installed_by_system,
                                 /*installed_by=*/{"@product"}, "user",
                                 tmp.path()));
  EXPECT_FALSE(CanProfileAndroid(TrustedInitiator(), uid_installed_by_system,
                                 /*installed_by=*/{"@product"}, "user",
                                 tmp.path()));

  // com.installer.package installer:
  EXPECT_TRUE(CanProfileAndroid(ShellInitiator(), uid_installed_by_store,
                                /*installed_by=*/{"com.installer.package"},
                                "user", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), uid_installed_by_store,
                                /*installed_by=*/{"com.installer.package"},
                                "user", tmp.path()));
  EXPECT_FALSE(CanProfileAndroid(ShellInitiator(), uid_installed_by_store,
                                 /*installed_by=*/{"@product"}, "user",
                                 tmp.path()));
  EXPECT_FALSE(CanProfileAndroid(TrustedInitiator(), uid_installed_by_store,
                                 /*installed_by=*/{"@product"}, "user",
                                 tmp.path()));
}

TEST(CanProfileAndroidTest, AppSandboxProcess) {
  unsigned uid_profileable_app = 10004;
  unsigned uid_nonprofileable_app = 10007;
  std::string content =
      PackageListLine(uid_profileable_app, /*debuggable=*/false,
                      /*profileable_from_shell=*/true,
                      /*profileable=*/true, /*installer=*/"@system");
  content +=  //
      PackageListLine(uid_nonprofileable_app, /*debuggable=*/false,
                      /*profileable_from_shell=*/false,
                      /*profileable=*/false, /*installer=*/"@system");
  auto tmp = base::TempFile::Create();
  base::WriteAll(tmp.fd(), content.c_str(), content.size());

  // Sandbox profileable if the app is profileable
  unsigned uid_profileable_sandbox = 20004;
  EXPECT_TRUE(CanProfileAndroid(ShellInitiator(), uid_profileable_sandbox,
                                /*installed_by=*/{}, "user", tmp.path()));
  EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), uid_profileable_sandbox,
                                /*installed_by=*/{}, "user", tmp.path()));

  unsigned uid_nonprofileable_sandbox = 20007;
  EXPECT_FALSE(CanProfileAndroid(ShellInitiator(), uid_nonprofileable_sandbox,
                                 /*installed_by=*/{}, "user", tmp.path()));
  EXPECT_FALSE(CanProfileAndroid(TrustedInitiator(), uid_nonprofileable_sandbox,
                                 /*installed_by=*/{}, "user", tmp.path()));
}

TEST(CanProfileAndroidTest, IsolatedProcess) {
  {
    // Packages list with only profileable packages
    unsigned uid_app = 10199;
    std::string content =
        PackageListLine(10003, /*debuggable=*/false,
                        /*profileable_from_shell=*/true,
                        /*profileable=*/true, /*installer=*/"@system");
    content +=  //
        PackageListLine(uid_app, /*debuggable=*/false,
                        /*profileable_from_shell=*/false,
                        /*profileable=*/true, /*installer=*/"@system");
    content +=  //
        PackageListLine(10008, /*debuggable=*/true,
                        /*profileable_from_shell=*/true,
                        /*profileable=*/true, /*installer=*/"@system");
    auto tmp = base::TempFile::Create();
    base::WriteAll(tmp.fd(), content.c_str(), content.size());

    // Any isolated process is thus profileable by trusted initiators
    unsigned uid_isolated = 90100;
    EXPECT_FALSE(CanProfileAndroid(ShellInitiator(), uid_isolated,
                                   /*installed_by=*/{}, "user", tmp.path()));
    EXPECT_TRUE(CanProfileAndroid(TrustedInitiator(), uid_isolated,
                                  /*installed_by=*/{}, "user", tmp.path()));
  }
  {
    // Packages list with an opted out package
    unsigned uid_app = 10199;
    std::string content =
        PackageListLine(10003, /*debuggable=*/false,
                        /*profileable_from_shell=*/true,
                        /*profileable=*/true, /*installer=*/"@system");
    content +=  //
        PackageListLine(uid_app, /*debuggable=*/false,
                        /*profileable_from_shell=*/false,
                        /*profileable=*/true, /*installer=*/"@system");
    content +=  //
        PackageListLine(10008, /*debuggable=*/false,
                        /*profileable_from_shell=*/false,
                        /*profileable=*/false, /*installer=*/"@system");
    auto tmp = base::TempFile::Create();
    base::WriteAll(tmp.fd(), content.c_str(), content.size());

    // Conservatively conclude that an isolated process is not profileable
    unsigned uid_isolated = 90100;
    EXPECT_FALSE(CanProfileAndroid(ShellInitiator(), uid_isolated,
                                   /*installed_by=*/{}, "user", tmp.path()));
    EXPECT_FALSE(CanProfileAndroid(TrustedInitiator(), uid_isolated,
                                   /*installed_by=*/{}, "user", tmp.path()));
  }
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
