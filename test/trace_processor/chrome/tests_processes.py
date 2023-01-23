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

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ChromeProcesses(TestSuite):

  def test_chrome_processes(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/chrome_processes.sql');
        SELECT pid, name, process_type FROM chrome_process;
        """,
        out=Csv("""
        "pid","name","process_type"
        18250,"Renderer","Renderer"
        17547,"Browser","Browser"
        18277,"GPU Process","Gpu"
        17578,"Browser","Browser"
        """))

  def test_chrome_processes_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/chrome_processes.sql');
        SELECT pid, name, process_type FROM chrome_process;
        """,
        out=Path('chrome_processes_android_systrace.out'))

  def test_chrome_threads(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/chrome_processes.sql');
        SELECT tid, name, is_main_thread, canonical_name
        FROM chrome_thread
        ORDER BY tid, name;
        """,
        out=Path('chrome_threads.out'))

  def test_chrome_threads_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/chrome_processes.sql');
        SELECT tid, name, is_main_thread, canonical_name
        FROM chrome_thread
        ORDER BY tid, name;
        """,
        out=Path('chrome_threads_android_systrace.out'))

  def test_chrome_processes_type(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
        SELECT pid, name, string_value AS chrome_process_type
        FROM
          process
        JOIN
          (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
          ON
            process.arg_set_id = chrome_process_args.arg_set_id
        ORDER BY pid;
        """,
        out=Csv("""
        "pid","name","chrome_process_type"
        17547,"Browser","Browser"
        17578,"Browser","Browser"
        18250,"Renderer","Renderer"
        18277,"GPU Process","Gpu"
        """))

  def test_chrome_processes_type_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query="""
        SELECT pid, name, string_value AS chrome_process_type
        FROM
          process
        JOIN
          (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
          ON
            process.arg_set_id = chrome_process_args.arg_set_id
        ORDER BY pid;
        """,
        out=Path('chrome_processes_type_android_systrace.out'))

  def test_track_with_chrome_process(self):
    return DiffTestBlueprint(
        trace=Path('track_with_chrome_process.textproto'),
        query="""
        SELECT pid, name, string_value AS chrome_process_type
        FROM
          process
        JOIN
          (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
          ON
            process.arg_set_id = chrome_process_args.arg_set_id
        ORDER BY pid;
        """,
        out=Csv("""
        "pid","name","chrome_process_type"
        5,"p5","[NULL]"
        """))

  def test_chrome_missing_processes_default_trace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query="""
        SELECT upid, pid, reliable_from
        FROM
          experimental_missing_chrome_processes
        JOIN
          process
          USING(upid)
        ORDER BY upid;
        """,
        out=Csv("""
        "upid","pid","reliable_from"
        """))

  def test_chrome_missing_processes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes.textproto'),
        query="""
        SELECT upid, pid, reliable_from
        FROM
          experimental_missing_chrome_processes
        JOIN
          process
          USING(upid)
        ORDER BY upid;
        """,
        out=Csv("""
        "upid","pid","reliable_from"
        2,100,1000000000
        3,1000,"[NULL]"
        """))

  def test_chrome_missing_processes_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes.textproto'),
        query="""
        SELECT arg_set_id, key, int_value
        FROM
          slice
        JOIN
          args
          USING(arg_set_id)
        ORDER BY arg_set_id, key;
        """,
        out=Csv("""
        "arg_set_id","key","int_value"
        2,"chrome_active_processes.pid[0]",10
        2,"chrome_active_processes.pid[1]",100
        2,"chrome_active_processes.pid[2]",1000
        """))

  def test_chrome_missing_processes_2(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes_extension.textproto'),
        query="""
        SELECT upid, pid, reliable_from
        FROM
          experimental_missing_chrome_processes
        JOIN
          process
          USING(upid)
        ORDER BY upid;
        """,
        out=Csv("""
        "upid","pid","reliable_from"
        2,100,1000000000
        3,1000,"[NULL]"
        """))

  def test_chrome_missing_processes_extension_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes_extension.textproto'),
        query="""
        SELECT arg_set_id, key, int_value
        FROM
          slice
        JOIN
          args
          USING(arg_set_id)
        ORDER BY arg_set_id, key;
        """,
        out=Csv("""
        "arg_set_id","key","int_value"
        2,"active_processes.pid[0]",10
        2,"active_processes.pid[1]",100
        2,"active_processes.pid[2]",1000
        """))
