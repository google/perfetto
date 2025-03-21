# Copyright (C) 2019 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# This file is used only in standalone builds. This file is ignored both in
# embedder builds (i.e. when other projects pull perfetto under /third_party/
# or similar) and in google internal builds.

workspace(name = "perfetto")

new_local_repository(
    name = "perfetto_cfg",
    path = "bazel/standalone",
    build_file_content = "",
)

# We already have the same setup for 'rules_android', see 'MODULE.bazel' file,
# however, 'rules_jvm_external' don't support Android SDK being set by
# 'rules_android'. So we need to set it here as well.
android_sdk_repository(
    name = "androidsdk",
    api_level = 35,
    build_tools_version = "35.0.1",
)

# We can't setup 'rules_jvm_external' in 'MODULE.bazel' because of bug:
# https://github.com/bazel-contrib/rules_jvm_external/issues/1320

# Order for 'load' and 'setup' statements is important, do not change!
load("@rules_jvm_external//:repositories.bzl", "rules_jvm_external_deps")

rules_jvm_external_deps()

load("@rules_jvm_external//:setup.bzl", "rules_jvm_external_setup")

rules_jvm_external_setup()

load("@rules_jvm_external//:defs.bzl", "maven_install")

maven_install(
    artifacts = [
        "androidx.test:runner:1.6.2",
        "androidx.test:monitor:1.7.2",
        "com.google.truth:truth:1.4.4",
        "junit:junit:4.13.2",
        "androidx.test.ext:junit:1.2.1",
    ],
    repositories = [
        "https://maven.google.com",
        "https://repo1.maven.org/maven2",
    ],
)

load("@perfetto//bazel:deps.bzl", "perfetto_deps")
perfetto_deps()

load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")
protobuf_deps()