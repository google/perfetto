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

load("@rules_android//android:rules.bzl", "ApkInfo", "StarlarkApkInfo")

def _get_apk_info(apk):
    if StarlarkApkInfo in apk:
        return apk[StarlarkApkInfo].signed_apk
    return apk[ApkInfo].signed_apk

def _android_instrumentation_test_impl(ctx):
    apk_file = _get_apk_info(ctx.attr.app)
    test_apk_file = _get_apk_info(ctx.attr.test_app)
    adb_file = ctx.attr._adb_exe.files_to_run.executable
    aapt2_file = ctx.attr._aapt2_exe.files_to_run.executable
    subst = dict()
    subst["%%adb_tool_path%%"] = adb_file.short_path
    subst["%%appt2_tool_path%%"] = aapt2_file.short_path
    subst["%%apk_path%%"] = apk_file.short_path
    subst["%%test_apk_path%%"] = test_apk_file.short_path
    out = ctx.actions.declare_file(ctx.label.name + "_script.sh")
    ctx.actions.expand_template(
        template = ctx.file._sh_script_template,
        is_executable = True,
        substitutions = subst,
        output = out,
    )
    runfiles = ctx.runfiles(
        files = [apk_file, test_apk_file],
        transitive_files = ctx.attr._adb_exe.files,
    ).merge(ctx.attr._aapt2_exe.default_runfiles)
    return [DefaultInfo(executable = out, runfiles = runfiles)]

android_instrumentation_test = rule(
    implementation = _android_instrumentation_test_impl,
    attrs = {
        "app": attr.label(
            providers = [[ApkInfo], [StarlarkApkInfo]],
            allow_files = True,
        ),
        "test_app": attr.label(
            providers = [[ApkInfo], [StarlarkApkInfo]],
            allow_files = True,
        ),
        "_adb_exe": attr.label(
            allow_single_file = True,
            cfg = "exec",
            default = Label("@androidsdk//:platform-tools/adb"),
        ),
        "_aapt2_exe": attr.label(
            executable = True,
            cfg = "exec",
            default = Label("@androidsdk//:aapt2_binary"),
        ),
        "_sh_script_template": attr.label(
            allow_single_file = True,
            default = Label(":run_ait_with_adb.sh"),
        ),
    },
    test = True,
)
