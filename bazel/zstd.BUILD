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
#
# Overlay for the legacy WORKSPACE path only; bzlmod uses the BCR module (see
# MODULE.bazel). Needed because upstream zstd ships no Bazel BUILD
# (facebook/zstd#3123), so new_git_repository has no //:zstd target.

load("@perfetto_cfg//:perfetto_cfg.bzl", "PERFETTO_CONFIG")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "zstd",
    srcs = glob([
        "lib/common/*.c",
        "lib/common/*.h",
        "lib/compress/*.c",
        "lib/compress/*.h",
        "lib/decompress/*.c",
        "lib/decompress/*.h",
        "lib/decompress/*.S",
        "lib/dictBuilder/*.c",
        "lib/dictBuilder/*.h",
    ]),
    hdrs = [
        "lib/zdict.h",
        "lib/zstd.h",
        "lib/zstd_errors.h",
    ],
    copts = [
        "-Wno-unused-function",
    ] + PERFETTO_CONFIG.deps_copts.zstd,
    # Disable the inline assembly to match the standalone GN build (which sets
    # ZSTD_DISABLE_ASM on x64) and keep the build portable across toolchains.
    defines = [
        "ZSTD_DISABLE_ASM",
    ],
    includes = [
        "lib",
    ],
    visibility = ["//visibility:public"],
)
