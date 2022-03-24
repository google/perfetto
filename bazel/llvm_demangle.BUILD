# Copyright (C) 2022 The Android Open Source Project
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
    name = "llvm_demangle",
    srcs = [
      "llvm/lib/Demangle/DLangDemangle.cpp",
      "llvm/lib/Demangle/Demangle.cpp",
      "llvm/lib/Demangle/ItaniumDemangle.cpp",
      "llvm/lib/Demangle/MicrosoftDemangle.cpp",
      "llvm/lib/Demangle/MicrosoftDemangleNodes.cpp",
      "llvm/lib/Demangle/RustDemangle.cpp",
    ],
    hdrs = [
      "llvm/include/llvm/Demangle/Demangle.h",
      "llvm/include/llvm/Demangle/DemangleConfig.h",
      "llvm/include/llvm/Demangle/ItaniumDemangle.h",
      "llvm/include/llvm/Demangle/MicrosoftDemangle.h",
      "llvm/include/llvm/Demangle/MicrosoftDemangleNodes.h",
      "llvm/include/llvm/Demangle/StringView.h",
      "llvm/include/llvm/Demangle/Utility.h",
    ],
    copts = ["-std=c++14"] + PERFETTO_CONFIG.deps_copts.llvm_demangle,
    includes = ["llvm/include"],
    visibility = ["//visibility:public"],
)
