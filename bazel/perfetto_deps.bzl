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

"""Module extension for Perfetto's third-party dependencies."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "new_git_repository")

def _perfetto_deps_impl(module_ctx):
    http_archive(
        name = "perfetto_dep_sqlite",
        url = "https://storage.googleapis.com/perfetto/sqlite-amalgamation-3500300.zip",
        sha256 = "9ad6d16cbc1df7cd55c8b55127c82a9bca5e9f287818de6dc87e04e73599d754",
        strip_prefix = "sqlite-amalgamation-3500300",
        build_file = "@perfetto//bazel:sqlite.BUILD",
    )

    http_archive(
        name = "perfetto_dep_sqlite_src",
        url = "https://storage.googleapis.com/perfetto/sqlite-src-3500300.zip",
        sha256 = "119862654b36e252ac5f8add2b3d41ba03f4f387b48eb024956c36ea91012d3f",
        strip_prefix = "sqlite-src-3500300",
        build_file = "@perfetto//bazel:sqlite.BUILD",
    )

    new_git_repository(
        name = "perfetto_dep_linenoise",
        remote = "https://fuchsia.googlesource.com/third_party/linenoise.git",
        commit = "c894b9e59f02203dbe4e2be657572cf88c4230c3",
        build_file = "@perfetto//bazel:linenoise.BUILD",
    )

    new_git_repository(
        name = "perfetto_dep_jsoncpp",
        remote = "https://github.com/open-source-parsers/jsoncpp",
        commit = "6aba23f4a8628d599a9ef7fa4811c4ff6e4070e2",  # v1.9.3
        build_file = "@perfetto//bazel:jsoncpp.BUILD",
    )

    new_git_repository(
        name = "perfetto_dep_expat",
        remote = "https://github.com/libexpat/libexpat",
        commit = "fa75b96546c069d17b8f80d91e0f4ef0cde3790d",  # R_2_6_2
        build_file = "@perfetto//bazel:expat.BUILD",
    )

    new_git_repository(
        name = "perfetto_dep_zlib",
        remote = "https://chromium.googlesource.com/chromium/src/third_party/zlib.git",
        commit = "6f9b4e61924021237d474569027cfb8ac7933ee6",
        build_file = "@perfetto//bazel:zlib.BUILD",
    )

    http_archive(
        name = "perfetto_dep_llvm_demangle",
        url = "https://storage.googleapis.com/perfetto/llvm-project-3b4c59c156919902c785ce3cbae0eee2ee53064d.tgz",
        sha256 = "f4a52e7f36edd7cacc844d5ae0e5f60b6f57c5afc40683e99f295886c9ce8ff4",
        strip_prefix = "llvm-project",
        build_file = "@perfetto//bazel:llvm_demangle.BUILD",
    )

    new_git_repository(
        name = "perfetto_dep_open_csd",
        remote = "https://android.googlesource.com/platform/external/OpenCSD.git",
        commit = "0ce01e934f95efb6a216a6efa35af1245151c779",
        build_file = "@perfetto//bazel:open_csd.BUILD",
    )

    return module_ctx.extension_metadata(
        reproducible = True,
        root_module_direct_deps = [
            "perfetto_dep_sqlite",
            "perfetto_dep_sqlite_src",
            "perfetto_dep_linenoise",
            "perfetto_dep_jsoncpp",
            "perfetto_dep_expat",
            "perfetto_dep_zlib",
            "perfetto_dep_llvm_demangle",
            "perfetto_dep_open_csd",
        ],
        root_module_direct_dev_deps = [],
    )

perfetto_deps = module_extension(
    implementation = _perfetto_deps_impl,
)
