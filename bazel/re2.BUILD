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
    name = "re2",
    srcs = [
        "re2/bitmap256.cc",
        "re2/bitmap256.h",
        "re2/bitstate.cc",
        "re2/compile.cc",
        "re2/dfa.cc",
        "re2/filtered_re2.cc",
        "re2/mimics_pcre.cc",
        "re2/nfa.cc",
        "re2/onepass.cc",
        "re2/parse.cc",
        "re2/perl_groups.cc",
        "re2/pod_array.h",
        "re2/prefilter.cc",
        "re2/prefilter.h",
        "re2/prefilter_tree.cc",
        "re2/prefilter_tree.h",
        "re2/prog.cc",
        "re2/prog.h",
        "re2/re2.cc",
        "re2/regexp.cc",
        "re2/regexp.h",
        "re2/set.cc",
        "re2/simplify.cc",
        "re2/sparse_array.h",
        "re2/sparse_set.h",
        "re2/tostring.cc",
        "re2/unicode_casefold.cc",
        "re2/unicode_casefold.h",
        "re2/unicode_groups.cc",
        "re2/unicode_groups.h",
        "re2/walker-inl.h",
        "util/rune.cc",
        "util/strutil.cc",
        "util/strutil.h",
        "util/utf.h",
    ],
    hdrs = [
        "re2/filtered_re2.h",
        "re2/re2.h",
        "re2/set.h",
        "re2/stringpiece.h",
    ],
    copts = [
        "-Wno-unused-function",
    ] + PERFETTO_CONFIG.deps_copts.re2,
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        "@perfetto_dep_abseil_cpp//:abseil_cpp",
    ],
)
