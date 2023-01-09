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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Process_tracking(DiffTestModule):

  def test_process_tracking(self):
    return DiffTestBlueprint(
        trace=Path('synth_process_tracking.py'),
        query=Path('../common/process_tracking_test.sql'),
        out=Path('process_tracking.out'))

  def test_process_tracking_process_tracking_short_lived_1(self):
    return DiffTestBlueprint(
        trace=Path('process_tracking_short_lived_1.py'),
        query=Path('../common/process_tracking_test.sql'),
        out=Path('process_tracking_process_tracking_short_lived_1.out'))

  def test_process_tracking_process_tracking_short_lived_2(self):
    return DiffTestBlueprint(
        trace=Path('process_tracking_short_lived_2.py'),
        query=Path('../common/process_tracking_test.sql'),
        out=Path('process_tracking_process_tracking_short_lived_2.out'))

  def test_process_tracking_uid(self):
    return DiffTestBlueprint(
        trace=Path('synth_process_tracking.py'),
        query=Path('process_tracking_uid_test.sql'),
        out=Path('process_tracking_uid.out'))

  def test_process_tracking_process_tracking_exec(self):
    return DiffTestBlueprint(
        trace=Path('process_tracking_exec.py'),
        query=Path('../common/process_tracking_test.sql'),
        out=Path('process_tracking_process_tracking_exec.out'))

  def test_process_parent_pid_process_parent_pid_tracking_1(self):
    return DiffTestBlueprint(
        trace=Path('process_parent_pid_tracking_1.py'),
        query=Path('process_parent_pid_test.sql'),
        out=Path('process_parent_pid_process_parent_pid_tracking_1.out'))

  def test_process_parent_pid_process_parent_pid_tracking_2(self):
    return DiffTestBlueprint(
        trace=Path('process_parent_pid_tracking_2.py'),
        query=Path('process_parent_pid_test.sql'),
        out=Path('process_parent_pid_process_parent_pid_tracking_2.out'))

  def test_process_tracking_reused_thread_print(self):
    return DiffTestBlueprint(
        trace=Path('reused_thread_print.py'),
        query=Path('../common/process_tracking_test.sql'),
        out=Path('process_tracking_reused_thread_print.out'))

  def test_slice_with_pid_sde_tracing_mark_write(self):
    return DiffTestBlueprint(
        trace=Path('sde_tracing_mark_write.textproto'),
        query=Path('slice_with_pid_test.sql'),
        out=Path('slice_with_pid_sde_tracing_mark_write.out'))
