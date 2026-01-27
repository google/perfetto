# Copyright (C) 2026 The Android Open Source Project
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

load("@perfetto_cfg//:perfetto_cfg.bzl", "PERFETTO_CONFIG")

cc_library(
    name = "abseil_cpp",
    srcs = glob([
        "absl/**/*.cc",
    ], exclude = [
        "absl/**/*_test.cc",
        "absl/**/*_benchmark.cc",
        "absl/log/flags.cc", # Excluded in GN to avoid conflicts
    ]),
    hdrs = glob([
        "absl/**/*.h",
        "absl/**/*.inc",
    ]),
    copts = [
        "-Wno-unused-function",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
)
