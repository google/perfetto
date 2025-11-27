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

load("@perfetto//bazel:proto_gen.bzl", "proto_descriptor_gen", "proto_gen")
load("@perfetto//bazel:run_ait_with_adb.bzl", "android_instrumentation_test")
load("@perfetto_cfg//:perfetto_cfg.bzl", "PERFETTO_CONFIG")
load("@rules_android//android:rules.bzl", "android_binary", "android_library")

# +----------------------------------------------------------------------------+
# | Base C++ rules.                                                            |
# +----------------------------------------------------------------------------+

def default_cc_args():
    return {
        "deps": PERFETTO_CONFIG.deps.build_config,
        "copts": [
            "-Wno-pragma-system-header-outside-header",
        ] + PERFETTO_CONFIG.default_copts,
        "cxxopts": PERFETTO_CONFIG.default_cxxopts,
        "includes": ["include"],
        "linkopts": select({
            "@perfetto//bazel:os_linux": ["-ldl", "-lrt", "-lpthread"],
            "@perfetto//bazel:os_freebsd": ["-ldl", "-lrt", "-lpthread"],
            "@perfetto//bazel:os_osx": [],
            "@perfetto//bazel:os_windows": ["ws2_32.lib"],
            "//conditions:default": ["-ldl"],
        }),
    }

def perfetto_build_config_cc_library(**kwargs):
    if not _rule_override("cc_library", **kwargs):
        native.cc_library(**kwargs)

def perfetto_filegroup(**kwargs):
    if not _rule_override("filegroup", **kwargs):
        native.filegroup(**kwargs)

def perfetto_genrule(**kwargs):
    if not _rule_override("genrule", **kwargs):
        native.genrule(**kwargs)

def perfetto_cc_library(**kwargs):
    args = _merge_dicts(default_cc_args(), kwargs)
    if not _rule_override("cc_library", **args):
        native.cc_library(**args)

def perfetto_cc_binary(**kwargs):
    args = _merge_dicts(default_cc_args(), kwargs)
    if not _rule_override("cc_binary", **args):
        native.cc_binary(**args)

def perfetto_py_binary(**kwargs):
    if not _rule_override("py_binary", **kwargs):
        native.py_binary(**kwargs)

def perfetto_py_library(**kwargs):
    if not _rule_override("py_library", **kwargs):
        native.py_library(**kwargs)

# +----------------------------------------------------------------------------+
# | Proto-related rules                                                        |
# +----------------------------------------------------------------------------+

def perfetto_proto_library(**kwargs):
    if not _rule_override("proto_library", **kwargs):
        native.proto_library(**kwargs)

def perfetto_cc_proto_library(**kwargs):
    if not _rule_override("cc_proto_library", **kwargs):
        native.cc_proto_library(**kwargs)

def perfetto_java_proto_library(**kwargs):
    if not _rule_override("java_proto_library", **kwargs):
        native.java_proto_library(**kwargs)

def perfetto_java_lite_proto_library(**kwargs):
    if not _rule_override("java_lite_proto_library", **kwargs):
        native.java_lite_proto_library(**kwargs)

# Unlike the other rules, this is an noop by default because Bazel does not
# support Go proto libraries.
def perfetto_dart_proto_library(**kwargs):
    _rule_override("dart_proto_library", **kwargs)

# Unlike the other rules, this is an noop by default because Bazel does not
# support Go proto libraries.
def perfetto_go_proto_library(**kwargs):
    _rule_override("go_proto_library", **kwargs)

# Unlike the other rules, this is an noop by default because Bazel does not
# support Python proto libraries.
def perfetto_py_proto_library(**kwargs):
    _rule_override("py_proto_library", **kwargs)

# Unlike the other rules, this is an noop by default because Bazel does not
# support Javascript/Typescript proto libraries.
def perfetto_jspb_proto_library(**kwargs):
    _rule_override("jspb_proto_library", **kwargs)

# +----------------------------------------------------------------------------+
# | Android-related rules                                                      |
# +----------------------------------------------------------------------------+
def perfetto_android_binary(**kwargs):
    if not _rule_override("android_binary", **kwargs):
        android_binary(**kwargs)

def perfetto_android_library(**kwargs):
    if not _rule_override("android_library", **kwargs):
        android_library(**kwargs)

def perfetto_android_jni_library(**kwargs):
    if not _rule_override("android_jni_library", **kwargs):
        _perfetto_android_jni_library(**kwargs)

def _perfetto_android_jni_library(
        name,
        binary_name,
        **input_cc_library_kwargs):
    # By default 'android_binary' rule merges all native libraries into one,
    # named '"lib%s.so" % android_binary_target_name'.
    # This is unsuitable for us: we want our native library to have a
    # predictable name to be able to load it from Java using
    # 'System.loadLibrary'.
    # To workaround this behaviour we wrap the 'cc_library'
    # target into 'cc_binary' that gives a name to the resulting library.
    # The same trick is done in 'android_jni_library' macro in google3.
    # See https://yaqs.corp.google.com/eng/q/1025522191808069632.
    #
    # 'binary_name' argument should be of pattern 'lib%s.so',
    # for this macro to be consistent with android_jni_library from google3.
    if not binary_name:
        fail("'binary_name' shouldn't be None")
    if not (binary_name.startswith("lib") and binary_name.endswith(".so")):
        fail("'binary_name' should sharts with 'lib' and ends with '.so'" +
             ", got %s instead" % binary_name)
    # We strip the name, since `native.cc_binary` adds prefix and suffix
    # to the generated library name.
    binary_target_name = binary_name.removeprefix("lib").removesuffix(".so")
    input_cc_library_name = name + "_input"
    # We add 'target_compatible_with = ' to all the cc_library targets to
    # exclude them from being build when invoke `bazel build :all`,
    # since these targets won't be able to compile anyway, see
    # https://bazel.build/docs/android-ndk#cclibrary-android.
    native.cc_library(
        name = input_cc_library_name,
        target_compatible_with = ["@platforms//os:android"],
        **input_cc_library_kwargs
    )
    native.cc_binary(
        name = binary_target_name,
        linkshared = True,
        deps = [input_cc_library_name],
        target_compatible_with = ["@platforms//os:android"],
    )
    native.cc_library(
        name = name,
        srcs = [binary_target_name],
        target_compatible_with = ["@platforms//os:android"],
    )

def perfetto_android_instrumentation_test(**kwargs):
    if not _rule_override("android_instrumentation_test", **kwargs):
        android_instrumentation_test(**kwargs)

# +----------------------------------------------------------------------------+
# | Misc rules.                                                                |
# +----------------------------------------------------------------------------+

# Generates .pbzero.{cc,h} from .proto(s). We deliberately do NOT generate
# conventional .pb.{cc,h} from here as protozero gen sources do not have any
# dependency on libprotobuf.
def perfetto_cc_protozero_library(name, deps, **kwargs):
    if _rule_override(
        "cc_protozero_library",
        name = name,
        deps = deps,
        **kwargs
    ):
        return

    # A perfetto_cc_protozero_library has two types of dependencies:
    # 1. Exactly one dependency on a proto_library target. This defines the
    #    .proto sources for the target
    # 2. Zero or more deps on other perfetto_cc_protozero_library targets. This
    #    to deal with the case of foo.proto including common.proto from another
    #    target.
    _proto_deps = [d for d in deps if d.endswith("_protos")]
    _cc_deps = [d for d in deps if d not in _proto_deps]
    if len(_proto_deps) != 1:
        fail("Too many proto deps for target %s" % name)

    args = {
        "name": name + "_src",
        "deps": _proto_deps,
        "suffix": "pbzero",
        "plugin": PERFETTO_CONFIG.root + ":protozero_plugin",
        "wrapper_namespace": "pbzero",
        "protoc": PERFETTO_CONFIG.deps.protoc[0],
        "root": PERFETTO_CONFIG.root,
    }
    if not _rule_override("proto_gen", **args):
        proto_gen(**args)

    perfetto_filegroup(
        name = name + "_h",
        srcs = [":" + name + "_src"],
        output_group = "h",
    )

    perfetto_cc_library(
        name = name,
        srcs = [":" + name + "_src"],
        hdrs = [":" + name + "_h"],
        deps = [PERFETTO_CONFIG.root + ":protozero"] + _cc_deps,
        **kwargs
    )

# Generates .ipc.{cc,h} and .pb.{cc.h} from .proto(s). The IPC sources depend
# on .pb.h so we need to generate also the standard protobuf sources here.
def perfetto_cc_ipc_library(name, deps, **kwargs):
    if _rule_override("cc_ipc_library", name = name, deps = deps, **kwargs):
        return

    # A perfetto_cc_ipc_library has two types of dependencies:
    # 1. Exactly one dependency on a proto_library target. This defines the
    #    .proto sources for the target
    # 2. Zero or more deps on other perfetto_cc_protocpp_library targets. This
    #    to deal with the case of foo.proto including common.proto from another
    #    target.
    _proto_deps = [d for d in deps if d.endswith("_protos")]
    _cc_deps = [d for d in deps if d not in _proto_deps]
    if len(_proto_deps) != 1:
        fail("Too many proto deps for target %s" % name)

    # Generates .ipc.{cc,h}.
    args = {
        "name": name + "_src",
        "deps": _proto_deps,
        "suffix": "ipc",
        "plugin": PERFETTO_CONFIG.root + ":ipc_plugin",
        "wrapper_namespace": "gen",
        "protoc": PERFETTO_CONFIG.deps.protoc[0],
        "root": PERFETTO_CONFIG.root,
    }
    if not _rule_override("proto_gen", **args):
        proto_gen(**args)

    perfetto_filegroup(
        name = name + "_h",
        srcs = [":" + name + "_src"],
        output_group = "h",
    )

    perfetto_cc_library(
        name = name,
        srcs = [":" + name + "_src"],
        hdrs = [":" + name + "_h"],
        deps = [
            # Generated .ipc.{cc,h} depend on this and protozero.
            PERFETTO_CONFIG.root + ":perfetto_ipc",
            PERFETTO_CONFIG.root + ":protozero",
        ] + _cc_deps,
        **kwargs
    )

# Generates .gen.{cc,h} from .proto(s).
def perfetto_cc_protocpp_library(name, deps, **kwargs):
    if _rule_override(
        "cc_protocpp_library",
        name = name,
        deps = deps,
        **kwargs
    ):
        return

    # A perfetto_cc_protocpp_library has two types of dependencies:
    # 1. Exactly one dependency on a proto_library target. This defines the
    #    .proto sources for the target
    # 2. Zero or more deps on other perfetto_cc_protocpp_library targets. This
    #    to deal with the case of foo.proto including common.proto from another
    #    target.
    _proto_deps = [d for d in deps if d.endswith("_protos")]
    _cc_deps = [d for d in deps if d not in _proto_deps]
    if len(_proto_deps) != 1:
        fail("Too many proto deps for target %s" % name)

    args = {
        "name": name + "_gen",
        "deps": _proto_deps,
        "suffix": "gen",
        "plugin": PERFETTO_CONFIG.root + ":cppgen_plugin",
        "wrapper_namespace": "gen",
        "protoc": PERFETTO_CONFIG.deps.protoc[0],
        "root": PERFETTO_CONFIG.root,
    }
    if not _rule_override("proto_gen", **args):
        proto_gen(**args)

    perfetto_filegroup(
        name = name + "_gen_h",
        srcs = [":" + name + "_gen"],
        output_group = "h",
    )

    # The headers from the gen plugin have implicit dependencies
    # on each other so will fail when compiled independently. Use
    # textual_hdrs to indicate this to Bazel.
    perfetto_cc_library(
        name = name,
        srcs = [":" + name + "_gen"],
        textual_hdrs = [":" + name + "_gen_h"],
        deps = [
            PERFETTO_CONFIG.root + ":protozero",
        ] + _cc_deps,
        **kwargs
    )

def perfetto_proto_descriptor(name, deps, outs, **kwargs):
    args = {
        "name": name,
        "deps": deps,
        "outs": outs,
    }
    if not _rule_override("proto_descriptor_gen", **args):
        proto_descriptor_gen(**args)

# Generator .descriptor.h from protos
def perfetto_cc_proto_descriptor(name, deps, outs, **kwargs):
    cmd = [
        "$(location gen_cc_proto_descriptor_py)",
        "--cpp_out=$@",
        "--gen_dir=$(GENDIR)",
        "$<",
    ]
    perfetto_genrule(
        name = name + "_gen",
        cmd = " ".join(cmd),
        tools = [
            ":gen_cc_proto_descriptor_py",
        ],
        srcs = deps,
        outs = outs,
    )

    perfetto_cc_library(
        name = name,
        hdrs = [":" + name + "_gen"],
        **kwargs
    )

def perfetto_protozero_descriptor_diff(name, minuend, subtrahend, outs, **kwargs):
    cmd = [
        "$(location src_protozero_descriptor_diff_protozero_descriptor_diff)",
        "--minuend=$(location " + minuend + ")",
        "--subtrahend=$(location " + subtrahend + ")",
        "--out",
        "$@",
    ]
    perfetto_genrule(
        name = name,
        cmd = " ".join(cmd),
        tools = [
            ":src_protozero_descriptor_diff_protozero_descriptor_diff",
        ],
        srcs = [
          minuend,
          subtrahend,
        ],
        outs = outs,
        **kwargs
    )

def perfetto_cc_amalgamated_sql(name, deps, outs, namespace, **kwargs):
    if PERFETTO_CONFIG.root[:2] != "//":
        fail("Expected PERFETTO_CONFIG.root to start with //")

    genrule_tool = kwargs.pop("genrule_tool", ":gen_amalgamated_sql_py")
    cmd = [
        "$(location " + genrule_tool + ")",
        "--namespace",
        namespace,
        "--cpp-out=$@",
        "$(SRCS)",
    ]

    root_dir = kwargs.pop("root_dir", None)
    if root_dir:
        cmd += [
            "--root-dir",
            root_dir,
        ]

    perfetto_genrule(
        name = name + "_gen",
        cmd = " ".join(cmd),
        tools = [
            genrule_tool,
        ],
        srcs = deps,
        outs = outs,
    )
    perfetto_cc_library(
        name = name,
        hdrs = [":" + name + "_gen"],
        **kwargs
    )

def perfetto_cc_tp_tables(name, srcs, outs, deps = [], **kwargs):
    if PERFETTO_CONFIG.root[:2] != "//":
        fail("Expected PERFETTO_CONFIG.root to start with //")

    if PERFETTO_CONFIG.root == "//":
        python_path = PERFETTO_CONFIG.root + "python"
    else:
        python_path = PERFETTO_CONFIG.root + "/python"

    perfetto_py_library(
        name = name + "_lib",
        deps = [
            python_path + ":trace_processor_table_generator",
        ],
        srcs = srcs,
    )

    perfetto_py_binary(
        name = name + "_tool",
        deps = [
            ":" + name + "_lib",
            python_path + ":trace_processor_table_generator",
        ] + [d + "_lib" for d in deps],
        srcs = [
            "tools/gen_tp_table_headers.py",
        ],
        main = "tools/gen_tp_table_headers.py",
        python_version = "PY3",
    )

    cmd = ["$(location " + name + "_tool)"]
    cmd += ["--gen-dir", "$(RULEDIR)"]
    # Do not use $(SRCS) expansion for --inputs.
    # Arguments given to --inputs are converted into python modules, using
    # brittle string manipulation. Instead, do the string manipulation here.
    # If $(SRCS) expansion is used, filepaths can contain a path prefix, making
    # it difficult to get the correct python module name.
    # The py_library above, ${name}_lib, bundles python modules to be
    # available on the python path for the py_binary, ${name}_tool.
    module_names = [src.replace("/", ".").removesuffix(".py") for src in srcs]
    cmd += ["--inputs", " ".join(module_names)]
    if PERFETTO_CONFIG.root != "//":
        cmd += ["--import-prefix", PERFETTO_CONFIG.root[2:]]

    perfetto_genrule(
        name = name + "_gen",
        cmd = " ".join(cmd),
        tools = [
            ":" + name + "_tool",
        ],
        srcs = srcs,
        outs = outs,
    )

    perfetto_filegroup(
        name = name,
        srcs = [":" + name + "_gen"],
        **kwargs
    )

# +----------------------------------------------------------------------------+
# | Misc utility functions                                                     |
# +----------------------------------------------------------------------------+

def _rule_override(rule_name, **kwargs):
    overrides = getattr(PERFETTO_CONFIG, "rule_overrides", struct())
    overridden_rule = getattr(overrides, rule_name, None)
    if overridden_rule:
        overridden_rule(**kwargs)
        return True
    return False

def _merge_dicts(*args):
    res = {}
    for arg in args:
        for k, v in arg.items():
            if type(v) == "string" or type(v) == "bool":
                res[k] = v
            elif type(v) == "list" or type(v) == "select":
                res[k] = res.get(k, []) + v
            else:
                fail("key type not supported: " + type(v))
    return res
