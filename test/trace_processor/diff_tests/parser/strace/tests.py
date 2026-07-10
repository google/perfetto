#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class StraceParser(TestSuite):

  def test_strace_basic_slices(self):
    return DiffTestBlueprint(
        trace=Path('basic.strace'),
        query="""
        SELECT ts, dur, name
        FROM slice
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","name"
        52321000000000,0,"openat"
        52321000100000,0,"read"
        52321000200000,0,"close"
        """))

  def test_strace_basic_args(self):
    return DiffTestBlueprint(
        trace=Path('basic.strace'),
        query="""
        SELECT s.name, a.key, a.string_value
        FROM slice s
        JOIN args a ON s.arg_set_id = a.arg_set_id
        WHERE s.name = 'openat'
        ORDER BY a.key;
        """,
        out=Csv("""
        "name","key","string_value"
        "openat","args","AT_FDCWD, "/etc/passwd", O_RDONLY"
        "openat","ret","3"
        """))

  def test_strace_unfinished_resumed(self):
    return DiffTestBlueprint(
        trace=Path('unfinished_resumed.strace'),
        query="""
        SELECT ts, dur, name
        FROM slice
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","name"
        52321000000000,500000,"futex"
        52321000600000,0,"write"
        """))
