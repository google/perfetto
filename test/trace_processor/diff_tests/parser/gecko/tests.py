#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import Json
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
          13260,"[NULL]","__start_thread","/apex/com.android.runtime/lib64/bionic/libc.so",0,5551
          13261,13260,"__pthread_start(void*)","/apex/com.android.runtime/lib64/bionic/libc.so",0,5551
          13262,13261,"art::Thread::CreateCallbackWithUffdGc(void*)","/apex/com.android.art/lib64/libart.so",0,3043
          13263,13262,"art::Thread::CreateCallback(void*)","/apex/com.android.art/lib64/libart.so",2,3043
          13266,13263,"art::ArtMethod::Invoke(art::Thread*, unsigned int*, unsigned int, art::JValue*, char const*)","/apex/com.android.art/lib64/libart.so",0,3036
          13267,13266,"art_quick_invoke_stub","/apex/com.android.art/lib64/libart.so",0,3036
          13268,13267,"java.lang.Thread.run","/system/framework/arm64/boot.oat",0,2159
          0,"[NULL]","__libc_init","/apex/com.android.runtime/lib64/bionic/libc.so",0,1714
          1,0,"main","/system/bin/app_process64",0,1714
          2,1,"android::AndroidRuntime::start(char const*, android::Vector<android::String8> const&, bool)","/system/lib64/libandroid_runtime.so",0,1714
        '''))

  def test_gecko_preprocessed_format(self):
    return DiffTestBlueprint(
        trace=DataPath('gecko_preprocessed_dav1d.json'),
        query="""
          INCLUDE PERFETTO MODULE stacks.cpu_profiling;

          SELECT name, mapping_name, self_count, cumulative_count
          FROM cpu_profiling_summary_tree
          ORDER BY cumulative_count desc
          LIMIT 10
        """,
        out=Csv('''
          "name","mapping_name","self_count","cumulative_count"
          "start","gecko",0,54917
          "main","gecko",0,54917
          "dav1d_send_data","gecko",0,54514
          "gen_picture","gecko",1,54508
          "dav1d_parse_obus","gecko",1,54507
          "dav1d_submit_frame","gecko",4,54488
          "dav1d_decode_frame","gecko",0,54480
          "dav1d_decode_frame_main","gecko",6,54419
          "dav1d_decode_tile_sbrow","gecko",32,40172
          "decode_sb","gecko",13,40088
        '''))

  def test_gecko_shared_resources(self):
    return DiffTestBlueprint(
        trace=DataPath('gecko_shared_resources.json.gz'),
        query="""
          INCLUDE PERFETTO MODULE stacks.cpu_profiling;

          SELECT
            t.name as thread_name,
            count(*) as sample_count
          FROM cpu_profile_stack_sample s
          JOIN thread t USING(utid)
          GROUP BY t.name
          ORDER BY sample_count desc
        """,
        out=Csv('''
          "thread_name","sample_count"
          "benches",863
          "cargo",56
          "tokio-runtime-worker",16
          "Thread <27885288>",5
          "Thread <27885276>",3
          "Thread <27885290>",2
          "Thread <27885289>",2
          "Thread <27885277>",2
        '''))

  # Hand-crafted minimal preprocessed-format profile exercising all four
  # Firefox marker phases on a single thread:
  #  - phase 0 (Instant)         => zero-duration slice
  #  - phase 1 (Interval)        => slice with computed duration
  #  - phase 2/3 (Start/End pair) => single slice spanning [startTime, endTime]
  # Also verifies category resolution from `meta.categories` and that the
  # `data` payload is flattened into `data.*` args.
  def test_gecko_markers(self):
    return DiffTestBlueprint(
        trace=Json(contents="""{
          "meta": {
            "categories": [
              {"name": "Other", "color": "grey", "subcategories": ["Other"]},
              {"name": "DOM", "color": "blue", "subcategories": ["Other"]}
            ]
          },
          "threads": [
            {
              "name": "main",
              "tid": 100,
              "pid": 100,
              "stringArray": ["Click", "Boot", "DOMEvent"],
              "frameTable": {"func": []},
              "funcTable": {"name": []},
              "stackTable": {"prefix": [], "frame": []},
              "samples": {"stack": [], "time": []},
              "markers": {
                "name":      [0,    1,    2,    2],
                "startTime": [10.0, 20.0, 30.0, 0.0],
                "endTime":   [10.0, 25.0, 0.0,  35.0],
                "phase":     [0,    1,    2,    3],
                "category":  [0,    0,    1,    1],
                "data":      [{"flow": 7}, null, {"target": "btn"}, null],
                "length":    4
              }
            }
          ]
        }"""),
        query="""
          SELECT
            thread.name AS thread_name,
            slice.name,
            slice.ts,
            slice.dur,
            slice.category,
            IFNULL(EXTRACT_ARG(slice.arg_set_id, 'data.flow'), '') AS data_flow,
            IFNULL(EXTRACT_ARG(slice.arg_set_id, 'data.target'), '') AS data_target
          FROM slice
          JOIN thread_track ON slice.track_id = thread_track.id
          JOIN thread USING(utid)
          ORDER BY slice.ts
        """,
        out=Csv('''
          "thread_name","name","ts","dur","category","data_flow","data_target"
          "main","Click",10000000,0,"Other",7,""
          "main","Boot",20000000,5000000,"Other","",""
          "main","DOMEvent",30000000,5000000,"DOM","","btn"
        '''))
