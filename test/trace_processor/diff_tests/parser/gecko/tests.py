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


class GeckoParser(TestSuite):

  def test_gecko_samples_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('trace_processor_perf_as_gecko.json'),
        query="""
          INCLUDE PERFETTO MODULE stacks.cpu_profiling;

          SELECT id, parent_id, name, mapping_name, self_count, cumulative_count
          FROM cpu_profiling_summary_tree
          LIMIT 10
        """,
        out=Csv('''
          "id","parent_id","name","mapping_name","self_count","cumulative_count"
          0,"[NULL]","__libc_start_call_main","/usr/lib/x86_64-linux-gnu/libc.so.6",0,37030
          1,0,"main","/usr/local/google/home/lalitm/perfetto/out/linux_clang_release/trace_processor_shell",0,37030
          2,1,"perfetto::trace_processor::(anonymous namespace)::TraceProcessorMain(int, char**)","/usr/local/google/home/lalitm/perfetto/out/linux_clang_release/trace_processor_shell",0,37030
          3,2,"perfetto::trace_processor::(anonymous namespace)::StartInteractiveShell(perfetto::trace_processor::(anonymous namespace)::InteractiveOptions const&)","/usr/local/google/home/lalitm/perfetto/out/linux_clang_release/trace_processor_shell",0,37029
          4,3,"read","/usr/lib/x86_64-linux-gnu/libc.so.6",8,8
          5,3,"cfree@GLIBC_2.2.5","/usr/lib/x86_64-linux-gnu/libc.so.6",1,1
          6,2,"clock_gettime@@GLIBC_2.17","/usr/lib/x86_64-linux-gnu/libc.so.6",1,1
          7,3,"perfetto::trace_processor::TraceProcessorImpl::ExecuteQuery(std::__Cr::basic_string<char, std::__Cr::char_traits<char>, std::__Cr::allocator<char> > const&)","/usr/local/google/home/lalitm/perfetto/out/linux_clang_release/trace_processor_shell",0,37020
          8,7,"perfetto::trace_processor::PerfettoSqlEngine::ExecuteUntilLastStatement(perfetto::trace_processor::SqlSource)","/usr/local/google/home/lalitm/perfetto/out/linux_clang_release/trace_processor_shell",0,37020
          9,8,"perfetto::trace_processor::PerfettoSqlEngine::ExecuteInclude(perfetto::trace_processor::PerfettoSqlParser::Include const&, perfetto::trace_processor::PerfettoSqlParser const&)","/usr/local/google/home/lalitm/perfetto/out/linux_clang_release/trace_processor_shell",0,37020
        '''))

  def test_gecko_samples_simpleperf_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf_as_gecko.json'),
        query="""
          INCLUDE PERFETTO MODULE stacks.cpu_profiling;

          SELECT id, parent_id, name, mapping_name, self_count, cumulative_count
          FROM cpu_profiling_summary_tree
          ORDER BY cumulative_count desc
          LIMIT 10
        """,
        out=Csv('''
          "id","parent_id","name","mapping_name","self_count","cumulative_count"
          0,"[NULL]","__libc_init","/apex/com.android.runtime/lib64/bionic/libc.so",6,7284
          1,0,"main","/system/bin/app_process64",5,7278
          2,1,"android::AndroidRuntime::start(char const*, android::Vector<android::String8> const&, bool)","/system/lib64/libandroid_runtime.so",7,7273
          3,2,"_JNIEnv::CallStaticVoidMethod(_jclass*, _jmethodID*, ...)","/system/lib64/libandroid_runtime.so",31,7266
          4,3,"art::JNI<true>::CallStaticVoidMethodV(_JNIEnv*, _jclass*, _jmethodID*, std::__va_list)","/apex/com.android.art/lib64/libart.so",5,7235
          5,4,"art::JValue art::InvokeWithVarArgs<_jmethodID*>(art::ScopedObjectAccessAlreadyRunnable const&, _jobject*, _jmethodID*, std::__va_list)","/apex/com.android.art/lib64/libart.so",9,7230
          6,5,"art_quick_invoke_static_stub","/apex/com.android.art/lib64/libart.so",14,7221
          7,6,"com.android.internal.os.ZygoteInit.main","/system/framework/arm64/boot-framework.oat",8,7207
          8,7,"com.android.internal.os.RuntimeInit$MethodAndArgsCaller.run","/system/framework/arm64/boot-framework.oat",9,7199
          9,8,"art_jni_trampoline","/system/framework/arm64/boot.oat",11,7190
        '''))
