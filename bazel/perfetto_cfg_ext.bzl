# Copyright (C) 2025 The Android Open Source Project
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

"""Module extension for Perfetto configuration.

This extension creates a @perfetto_cfg repository that provides the
PERFETTO_CONFIG struct used throughout the build.
"""

def _perfetto_cfg_repository_impl(repository_ctx):
    # Create a symlink to the standalone configuration
    repository_ctx.symlink(
        repository_ctx.path(Label("@perfetto//bazel/standalone:perfetto_cfg.bzl")),
        "perfetto_cfg.bzl",
    )
    repository_ctx.file("BUILD.bazel", """
# Auto-generated BUILD file for perfetto_cfg repository
exports_files(["perfetto_cfg.bzl"])
""")

_perfetto_cfg_repository = repository_rule(
    implementation = _perfetto_cfg_repository_impl,
)

def _perfetto_cfg_ext_impl(module_ctx):
    _perfetto_cfg_repository(name = "perfetto_cfg")
    return module_ctx.extension_metadata(
        reproducible = True,
        root_module_direct_deps = ["perfetto_cfg"],
        root_module_direct_dev_deps = [],
    )

perfetto_cfg_ext = module_extension(
    implementation = _perfetto_cfg_ext_impl,
)
