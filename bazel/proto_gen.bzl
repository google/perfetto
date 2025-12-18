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

# This file defines the proto_gen() rule that is used for generating protos
# with custom plugins (ipc and protozero).

def _proto_gen_impl(ctx):
    proto_src = [
        f
        for dep in ctx.attr.deps
        for f in dep[ProtoInfo].direct_sources
    ]
    includes = [
        f
        for dep in ctx.attr.deps
        for f in dep[ProtoInfo].transitive_imports.to_list()
    ]
    proto_paths = [
        f
        for dep in ctx.attr.deps
        for f in dep[ProtoInfo].transitive_proto_path.to_list()
    ]

    proto_path = "."

    out_dir = ctx.bin_dir.path
    strip_base_path = ""
    if ctx.attr.root != "//":
        # This path is hit in Google internal builds, where root is typically
        # //third_party/perfetto.
        proto_path = "."

        # The below will likely be //third_party/perfetto/ but may also be any
        # subdir under //third_party/perfetto.
        strip_base_path = ctx.label.package + "/"
    elif ctx.label.workspace_root:
        # This path is hit when proto targets are built as @perfetto//:xxx
        # instead of //:xxx. This happens in embedder builds.
        proto_path = ctx.label.workspace_root

        # We could be using the sibling repository layout, in which case we do nothing.
        if not ctx.label.workspace_root.startswith("../"):
            # workspace_root == "external/perfetto" and we need to rebase the paths
            # passed to protoc.
            out_dir += "/" + ctx.label.workspace_root
        strip_base_path = ctx.label.workspace_root + "/"


    out_files = []
    suffix = ctx.attr.suffix
    for src in proto_src:
        base_path = src.path[:-len(".proto")]
        if base_path.startswith(strip_base_path):
            base_path = base_path[len(strip_base_path):]
        out_files += [ctx.actions.declare_file(base_path + ".%s.h" % suffix)]
        out_files += [ctx.actions.declare_file(base_path + ".%s.cc" % suffix)]

    arguments = [
        "--proto_path=" + proto_path
        for proto_path in proto_paths
    ]

    plugin_deps = []
    if ctx.attr.plugin:
        wrap_arg = ctx.attr.wrapper_namespace
        arguments += [
            "--plugin=protoc-gen-plugin=" + ctx.executable.plugin.path,
            "--plugin_out=wrapper_namespace=" + wrap_arg + ":" + out_dir,
        ]
        plugin_deps += [ctx.executable.plugin]
    else:
        arguments += [
            "--cpp_out=lite=true:" + out_dir,
        ]

    arguments += [src.path for src in proto_src]
    ctx.actions.run(
        inputs = proto_src + includes + plugin_deps,
        tools = plugin_deps,
        outputs = out_files,
        executable = ctx.executable.protoc,
        arguments = arguments,
    )
    cc_files = depset([f for f in out_files if f.path.endswith(".cc")])
    h_files = depset([f for f in out_files if f.path.endswith(".h")])
    return [
        DefaultInfo(files = cc_files),
        OutputGroupInfo(
            cc = cc_files,
            h = h_files,
        ),
    ]


proto_gen = rule(
    attrs = {
        "deps": attr.label_list(
            mandatory = True,
            allow_empty = False,
            providers = [ProtoInfo],
        ),
        "plugin": attr.label(
            executable = True,
            mandatory = False,
            cfg = "host",
        ),
        "wrapper_namespace": attr.string(
            mandatory = False,
            default = ""
        ),
        "suffix": attr.string(
            mandatory = True,
        ),
        "protoc": attr.label(
            executable = True,
            cfg = "host",
        ),
        "root": attr.string(
            mandatory = False,
            default = "//",
        ),
    },
    implementation = _proto_gen_impl,
)


def _proto_descriptor_gen_impl(ctx):
    descriptors = [
        f
        for dep in ctx.attr.deps
        for f in dep[ProtoInfo].transitive_descriptor_sets.to_list()
    ]
    ctx.actions.run_shell(
        inputs=descriptors,
        outputs=ctx.outputs.outs,
        command='cat %s > %s' % (
            ' '.join([f.path for f in descriptors]), ctx.outputs.outs[0].path)
    )


proto_descriptor_gen = rule(
    implementation=_proto_descriptor_gen_impl,
    attrs = {
        "deps": attr.label_list(
            mandatory = True,
            allow_empty = False,
            providers = [ProtoInfo],
        ),
        "outs": attr.output_list(mandatory=True),
    }
)
