# Copyright (C) 2024 The Android Open Source Project
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
    name = "expat",
    hdrs = glob(["expat/lib/*.h"]),
    deps = [
        ":expat_impl",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "expat_impl",
    srcs = [
      "expat/lib/xmlparse.c",
      "expat/lib/xmlrole.c",
      "expat/lib/xmltok.c",
    ],
    hdrs = [
      "expat/lib/ascii.h",
      "expat/lib/asciitab.h",
      "expat/lib/expat.h",
      "expat/lib/expat_external.h",
      "expat/lib/iasciitab.h",
      "expat/lib/internal.h",
      "expat/lib/latin1tab.h",
      "expat/lib/nametab.h",
      "expat/lib/siphash.h",
      "expat/lib/utf8tab.h",
      "expat/lib/winconfig.h",
      "expat/lib/xmlrole.h",
      "expat/lib/xmltok.h",
      "expat/lib/xmltok_impl.c",
      "expat/lib/xmltok_impl.h",
      "expat/lib/xmltok_ns.c",
    ],
    deps = [
        "@perfetto//buildtools/expat/include:expat_config",
    ],
    copts = [
        "-DHAVE_EXPAT_CONFIG_H",
    ] + PERFETTO_CONFIG.deps_copts.expat,
    defines = [
        "XML_STATIC"
    ],
    includes = [
        "expat",
        "expat/lib",
    ],
)
