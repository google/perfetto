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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class ChromeGeneral(DiffTestModule):

  def test_chrome_histogram_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_histogram_hashes.textproto'),
        query=Metric('chrome_histogram_hashes'),
        out=TextProto(r"""
[perfetto.protos.chrome_histogram_hashes]: {
  hash: 10
  hash: 20
}
"""))

  def test_chrome_user_event_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_user_event_hashes.textproto'),
        query=Metric('chrome_user_event_hashes'),
        out=TextProto(r"""
[perfetto.protos.chrome_user_event_hashes]: {
  action_hash: 10
  action_hash: 20
}

"""))

  def test_chrome_performance_mark_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_performance_mark_hashes.textproto'),
        query=Metric('chrome_performance_mark_hashes'),
        out=TextProto(r"""
[perfetto.protos.chrome_performance_mark_hashes]: {
  site_hash: 10
  site_hash: 20
  mark_hash: 100
  mark_hash: 200
}
"""))

  def test_chrome_reliable_range(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
12,"First slice for utid=2","[NULL]",2
"""))

  def test_chrome_reliable_range_cropping(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_cropping.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
10000,"Range of interest packet","[NULL]",2
"""))

  def test_chrome_reliable_range_missing_processes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_missing_processes.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
1011,"Missing process data for upid=2",2,1
"""))

  def test_chrome_slice_names(self):
    return DiffTestBlueprint(
        trace=Path('chrome_slice_names.textproto'),
        query=Metric('chrome_slice_names'),
        out=TextProto(r"""
[perfetto.protos.chrome_slice_names]: {
  chrome_version_code: 123
  slice_name: "Looper.Dispatch: class1"
  slice_name: "name2"
}
"""))

  def test_chrome_tasks(self):
    return DiffTestBlueprint(
        trace=DataPath(
            'chrome_page_load_all_categories_not_extended.pftrace.gz'),
        query="""
SELECT RUN_METRIC('chrome/chrome_tasks.sql');

SELECT full_name, task_type, count() AS count
FROM chrome_tasks
GROUP BY full_name, task_type
ORDER BY count DESC
LIMIT 50;
""",
        out=Path('chrome_tasks.out'))

  def test_top_level_java_choreographer_slices_top_level_java_chrome_tasks(
      self):
    return DiffTestBlueprint(
        trace=DataPath('top_level_java_choreographer_slices'),
        query="""
SELECT RUN_METRIC(
  'chrome/chrome_tasks_template.sql',
  'slice_table_name', 'slice',
  'function_prefix', ''
);

SELECT
  full_name,
  task_type
FROM chrome_tasks
WHERE category = "toplevel,Java"
AND ts < 263904000000000
GROUP BY full_name, task_type;
""",
        out=Path(
            'top_level_java_choreographer_slices_top_level_java_chrome_tasks_test.out'
        ))

  def test_chrome_stack_samples_for_task(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_stack_traces_symbolized_trace.pftrace'),
        query="""
SELECT RUN_METRIC('chrome/chrome_stack_samples_for_task.sql',
    'target_duration_ms', '0.000001',
    'thread_name', '"CrBrowserMain"',
    'task_name', '"sendTouchEvent"');

SELECT
  sample.description,
  sample.ts,
  sample.depth
FROM chrome_stack_samples_for_task sample
JOIN (
    SELECT
      ts,
      dur
    FROM slice
    WHERE ts = 696373965001470
) test_slice
ON sample.ts >= test_slice.ts
  AND sample.ts <= test_slice.ts + test_slice.dur
ORDER BY sample.ts, sample.depth;
""",
        out=Path('chrome_stack_samples_for_task_test.out'))

  def test_chrome_log_message(self):
    return DiffTestBlueprint(
        trace=Path('chrome_log_message.textproto'),
        query="""
SELECT utid, tag, msg FROM android_logs;
""",
        out=Csv("""
"utid","tag","msg"
1,"foo.cc:123","log message"
"""))

  def test_chrome_log_message_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_log_message.textproto'),
        query=Path('chrome_log_message_args_test.sql'),
        out=Csv("""
"log_message","function_name","file_name","line_number"
"log message","func","foo.cc",123
"""))

  def test_chrome_custom_navigation_tasks(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_custom_navigation_trace.gz'),
        query="""
SELECT RUN_METRIC('chrome/chrome_tasks.sql');

SELECT full_name, task_type, count() AS count
FROM chrome_tasks
WHERE full_name GLOB 'FrameHost::BeginNavigation*'
  OR full_name GLOB 'FrameHost::DidCommitProvisionalLoad*'
  OR full_name GLOB 'FrameHost::DidCommitSameDocumentNavigation*'
  OR full_name GLOB 'FrameHost::DidStopLoading*'
GROUP BY full_name, task_type
ORDER BY count DESC
LIMIT 50;
""",
        out=Csv("""
"full_name","task_type","count"
"FrameHost::BeginNavigation (SUBFRAME)","navigation_task",5
"FrameHost::DidStopLoading (SUBFRAME)","navigation_task",3
"FrameHost::BeginNavigation (PRIMARY_MAIN_FRAME)","navigation_task",1
"FrameHost::DidCommitProvisionalLoad (SUBFRAME)","navigation_task",1
"""))

  def test_proto_content(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_scroll_without_vsync.pftrace'),
        query=Path('proto_content_test.sql'),
        out=Path('proto_content.out'))
