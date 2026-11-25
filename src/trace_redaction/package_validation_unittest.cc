/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_redaction/package_validation.h"

#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {
namespace {

TEST(PackageValidationTest, RejectsMissingPackageName) {
  Context context;
  context.package_uid = 10146;
  EXPECT_FALSE(PackageValidation().Validate(context).ok());
}

TEST(PackageValidationTest, RejectsMissingPackageUid) {
  Context context;
  context.package_name = "com.example.target";
  EXPECT_FALSE(PackageValidation().Validate(context).ok());
}

TEST(PackageValidationTest, AcceptsPopulatedPackage) {
  Context context;
  context.package_name = "com.example.target";
  context.package_uid = 10146;
  ASSERT_OK(PackageValidation().Validate(context));
}

}  // namespace
}  // namespace perfetto::trace_redaction
