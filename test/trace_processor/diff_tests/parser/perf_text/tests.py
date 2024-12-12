#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PerfTextParser(TestSuite):

  def test_perf_text_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('trace_processor_perf_as_text.txt'),
        query="""
          INCLUDE PERFETTO MODULE stacks.cpu_profiling;

          SELECT id, parent_id, name, mapping_name, self_count, cumulative_count
          FROM cpu_profiling_summary_tree
          LIMIT 10
        """,
        out=Csv('''
          "id","parent_id","name","mapping_name","self_count","cumulative_count"
          0,"[NULL]","_start","/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",1,2
          1,0,"[unknown]","[unknown]",1,1
          2,"[NULL]","_dl_start","/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",1,1
          3,"[NULL]","_dl_start_user","/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",0,16
          4,3,"_dl_start","/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",2,5
          5,4,"[unknown]","[unknown]",3,3
          6,"[NULL]","[unknown]","[unknown]",0,27
          7,6,"[unknown]","[unknown]",0,3
          8,7,"__GI___tunables_init","/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",1,2
          9,8,"[unknown]","[unknown]",1,1
        '''))

  def test_perf_text_simpleperf_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf_as_text.txt'),
        query="""
          INCLUDE PERFETTO MODULE stacks.cpu_profiling;

          SELECT id, parent_id, name, mapping_name, self_count, cumulative_count
          FROM cpu_profiling_summary_tree
          LIMIT 10
        """,
        out=Csv('''
          "id","parent_id","name","mapping_name","self_count","cumulative_count"
          0,"[NULL]","__libc_init","/apex/com.android.runtime/lib64/bionic/libc.so",0,1714
          1,0,"main","/system/bin/app_process64",0,1714
          2,1,"android::AndroidRuntime::start(char const*, android::Vector<android::String8> const&, bool)","/system/lib64/libandroid_runtime.so",0,1714
          3,2,"_JNIEnv::CallStaticVoidMethod(_jclass*, _jmethodID*, ...)","/system/lib64/libandroid_runtime.so",0,1714
          4,3,"art::JNI<true>::CallStaticVoidMethodV(_JNIEnv*, _jclass*, _jmethodID*, std::__va_list)","/apex/com.android.art/lib64/libart.so",0,1714
          5,4,"art::JValue art::InvokeWithVarArgs<_jmethodID*>(art::ScopedObjectAccessAlreadyRunnable const&, _jobject*, _jmethodID*, std::__va_list)","/apex/com.android.art/lib64/libart.so",0,1714
          6,5,"art_quick_invoke_static_stub","/apex/com.android.art/lib64/libart.so",0,1714
          7,6,"com.android.internal.os.ZygoteInit.main","/system/framework/arm64/boot-framework.oat",0,1714
          8,7,"com.android.internal.os.RuntimeInit$MethodAndArgsCaller.run","/system/framework/arm64/boot-framework.oat",0,1714
          9,8,"art_jni_trampoline","/system/framework/arm64/boot.oat",0,1714
        '''))
