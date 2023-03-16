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

load("@perfetto_cfg//:perfetto_cfg.bzl", "PERFETTO_CONFIG")

cc_library(
    name = "zlib",
    srcs = [
        "adler32.c",
        "chromeconf.h",
        "compress.c",
        "contrib/optimizations/insert_string.h",
        "cpu_features.c",
        "cpu_features.h",
        "crc32.c",
        "crc32.h",
        "deflate.c",
        "deflate.h",
        "gzclose.c",
        "gzguts.h",
        "gzlib.c",
        "gzread.c",
        "gzwrite.c",
        "infback.c",
        "inffast.c",
        "inffast.h",
        "inffixed.h",
        "inflate.c",
        "inflate.h",
        "inftrees.c",
        "inftrees.h",
        "trees.c",
        "trees.h",
        "uncompr.c",
        "zconf.h",
        "zutil.c",
        "zutil.h",
    ],
    hdrs = [
        "zlib.h",
    ],
    copts = select({
      "@perfetto//bazel:os_windows": ["-DX86_WINDOWS"],
      "//conditions:default": [],
    }) + [
        "-Wno-unused-function",
        "-DZLIB_IMPLEMENTATION",
        "-DCHROMIUM_ZLIB_NO_CHROMECONF",
    ] + PERFETTO_CONFIG.deps_copts.zlib,
    includes = ["zlib"],
    visibility = ["//visibility:public"],
)
