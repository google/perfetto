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
load(
    "@perfetto//bazel:rules.bzl",
    "perfetto_py_binary",
    "perfetto_py_library",
)

licenses(["notice"])

package(default_visibility = ["//visibility:private"])

perfetto_py_binary(
    name = "trace_processor_py_example",
    srcs = ["example.py"],
    deps = [":trace_processor_py"] + PERFETTO_CONFIG.deps.pandas_py,
    main = "example.py",
    python_version = "PY3",
)

perfetto_py_library(
    name = "trace_processor_py",
    srcs = glob(["perfetto/trace_processor/*.py"]),
    data = [
        ":trace_uri_resolver",
        "perfetto/trace_processor/trace_processor.descriptor",
        "perfetto/trace_processor/metrics.descriptor",
        PERFETTO_CONFIG.root + ":trace_processor_shell",
    ],
    deps = PERFETTO_CONFIG.deps.tp_vendor_py +
        PERFETTO_CONFIG.deps.protobuf_py +
        PERFETTO_CONFIG.deps.pandas_py,
    imports = [
        ".",
    ],
    visibility = PERFETTO_CONFIG.public_visibility,
)

perfetto_py_library(
    name = "trace_uri_resolver",
    srcs = glob(["perfetto/trace_uri_resolver/*.py"]),
    imports = [
        ".",
    ],
)

perfetto_py_library(
    name = "experimental_slice_breakdown_lib",
    srcs = glob(["perfetto/experimental/slice_breakdown/*.py"]),
    deps = [
        ":trace_processor_py",
    ],
    imports = [
        "tools/slice_breakdown",
    ],
)

perfetto_py_binary(
    name = "experimental_slice_breakdown_bin",
    srcs = ["tools/slice_breakdown.py"],
    main = "tools/slice_breakdown.py",
    deps = [
        ":experimental_slice_breakdown_lib",
        ":trace_processor_py",
    ] + PERFETTO_CONFIG.deps.pandas_py,
    python_version = "PY3",
    legacy_create_init = 0,
)

perfetto_py_library(
    name = "batch_trace_processor",
    srcs = glob([
      "perfetto/batch_trace_processor/*.py"
    ]),
    deps = [
        ":trace_processor_py",
    ] + PERFETTO_CONFIG.deps.pandas_py +
        PERFETTO_CONFIG.deps.tp_vendor_py,
    imports = [
        ".",
    ],
)

perfetto_py_binary(
    name = "batch_trace_processor_shell",
    srcs = ["tools/batch_trace_processor_shell.py"],
    main = "tools/batch_trace_processor_shell.py",
    deps = [
        ":trace_processor_py",
        ":batch_trace_processor",
    ] + PERFETTO_CONFIG.deps.pandas_py,
    python_version = "PY3",
    legacy_create_init = 0,
)
