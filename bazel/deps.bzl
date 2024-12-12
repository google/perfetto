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

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "new_git_repository")

# This file must be kept in sync with tools/install-build-deps.

# To generate shallow_since fields for git repos, use:
#   git show --date=raw COMMIT

def perfetto_deps():
    # Note: this is more recent than the version of protobuf we use in the
    # GN and Android builds. This is because older versions of protobuf don't
    # support Bazel.
    _add_repo_if_not_existing(
        http_archive,
        name = "com_google_protobuf",
        strip_prefix = "protobuf-3.10.1",
        url = "https://github.com/protocolbuffers/protobuf/archive/v3.10.1.tar.gz",
        sha256 = "6adf73fd7f90409e479d6ac86529ade2d45f50494c5c10f539226693cb8fe4f7",
    )

    _add_repo_if_not_existing(
        http_archive,
        name = "perfetto_dep_sqlite",
        url = "https://storage.googleapis.com/perfetto/sqlite-amalgamation-3440200.zip",
        sha256 = "833be89b53b3be8b40a2e3d5fedb635080e3edb204957244f3d6987c2bb2345f",
        strip_prefix = "sqlite-amalgamation-3440200",
        build_file = "//bazel:sqlite.BUILD",
    )

    _add_repo_if_not_existing(
        http_archive,
        name = "perfetto_dep_sqlite_src",
        url = "https://storage.googleapis.com/perfetto/sqlite-src-3440200.zip",
        sha256 = "73187473feb74509357e8fa6cb9fd67153b2d010d00aeb2fddb6ceeb18abaf27",
        strip_prefix = "sqlite-src-3440200",
        build_file = "//bazel:sqlite.BUILD",
    )

    _add_repo_if_not_existing(
        new_git_repository,
        name = "perfetto_dep_linenoise",
        remote = "https://fuchsia.googlesource.com/third_party/linenoise.git",
        commit = "c894b9e59f02203dbe4e2be657572cf88c4230c3",
        build_file = "//bazel:linenoise.BUILD",
    )

    _add_repo_if_not_existing(
        new_git_repository,
        name = "perfetto_dep_jsoncpp",
        remote = "https://github.com/open-source-parsers/jsoncpp",
        commit = "6aba23f4a8628d599a9ef7fa4811c4ff6e4070e2",  # v1.9.3
        build_file = "//bazel:jsoncpp.BUILD",
    )

    _add_repo_if_not_existing(
        new_git_repository,
        name = "perfetto_dep_expat",
        remote = "https://github.com/libexpat/libexpat",
        commit = "fa75b96546c069d17b8f80d91e0f4ef0cde3790d",  # R_2_6_2
        build_file = "//bazel:expat.BUILD",
    )

    _add_repo_if_not_existing(
        http_archive,
        name = "perfetto_dep_zlib",
        url = "https://storage.googleapis.com/perfetto/zlib-6d3f6aa0f87c9791ca7724c279ef61384f331dfd.tar.gz",
        sha256 = "e9a1d6e8c936de68628ffb83a13d28a40cd6b2def2ad9378e8b951d4b8f4df18",
        build_file = "//bazel:zlib.BUILD",
    )

    _add_repo_if_not_existing(
        http_archive,
        name = "perfetto_dep_llvm_demangle",
        url = "https://storage.googleapis.com/perfetto/llvm-project-3b4c59c156919902c785ce3cbae0eee2ee53064d.tgz",
        sha256 = "f4a52e7f36edd7cacc844d5ae0e5f60b6f57c5afc40683e99f295886c9ce8ff4",
        strip_prefix = "llvm-project",
        build_file = "//bazel:llvm_demangle.BUILD",
    )

    # Without this protobuf.bzl fails. This seems a bug in protobuf_deps().
    _add_repo_if_not_existing(
        http_archive,
        name = "bazel_skylib",
        sha256 = "bc283cdfcd526a52c3201279cda4bc298652efa898b10b4db0837dc51652756f",
        urls = [
            "https://mirror.bazel.build/github.com/bazelbuild/bazel-skylib/releases/download/1.7.1/bazel-skylib-1.7.1.tar.gz",
            "https://github.com/bazelbuild/bazel-skylib/releases/download/1.7.1/bazel-skylib-1.7.1.tar.gz",
        ]
    )

def _add_repo_if_not_existing(repo_rule, name, **kwargs):
    if name not in native.existing_rules():
        repo_rule(name = name, **kwargs)

