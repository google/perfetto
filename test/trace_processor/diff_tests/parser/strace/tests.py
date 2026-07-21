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
        1700000052321000000,0,"openat"
        1700000052321100000,0,"read"
        1700000052321200000,0,"close"
        1700000052321300000,0,"openat"
        1700000052321400000,0,"write"
        """))

  def test_strace_basic_args(self):
    return DiffTestBlueprint(
        trace=Path('basic.strace'),
        query="""
        SELECT s.name, a.key, a.string_value
        FROM slice s
        JOIN args a ON s.arg_set_id = a.arg_set_id
        WHERE s.name = 'openat'
        ORDER BY s.ts, a.key;
        """,
        out=Csv("""
        "name","key","string_value"
        "openat","args","AT_FDCWD, "/etc/passwd", O_RDONLY"
        "openat","ret","3"
        "openat","args","AT_FDCWD, "/nope", O_RDONLY"
        "openat","ret","-1 ENOENT (No such file or directory)"
        """))

  def test_strace_dash_f_pid(self):
    # The "1234 <ts> write(...)" line in basic.strace was collected with
    # `strace -f`; its leading pid becomes the slice's thread, distinct
    # from the other (unprefixed, tid-1) syscalls in the same trace.
    return DiffTestBlueprint(
        trace=Path('basic.strace'),
        query="""
        SELECT s.name, t.tid
        FROM slice s
        JOIN thread_track tt ON s.track_id = tt.id
        JOIN thread t USING (utid)
        WHERE s.name = 'write';
        """,
        out=Csv("""
        "name","tid"
        "write",1234
        """))

  def test_strace_basic_parse_failures_counted(self):
    # basic.strace has 3 lines that aren't syscall events: a SIGCHLD
    # delivery line, a process-exit banner, and one line that isn't valid
    # strace output at all. All three should be counted, not silently
    # dropped.
    return DiffTestBlueprint(
        trace=Path('basic.strace'),
        query="""
        SELECT name, value
        FROM stats
        WHERE name = 'strace_parse_failure';
        """,
        out=Csv("""
        "name","value"
        "strace_parse_failure",3
        """))

  def test_strace_unsupported_timestamp_format_counted(self):
    # unsupported_timestamp.strace opens with a valid `-ttt` line (so the
    # trace sniffs as strace format) followed by two `-t`/`-tt` lines and
    # another valid `-ttt` line. The `-t`/`-tt` lines must be counted under
    # the dedicated stat, not the generic strace_parse_failure one.
    return DiffTestBlueprint(
        trace=Path('unsupported_timestamp.strace'),
        query="""
        SELECT name, value
        FROM stats
        WHERE name IN ('strace_parse_failure',
                        'strace_unsupported_timestamp_format');
        """,
        out=Csv("""
        "name","value"
        "strace_parse_failure",0
        "strace_unsupported_timestamp_format",2
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
        1700000052321000000,500000,"futex"
        1700000052321600000,0,"write"
        """))

  def test_strace_resumed_then_unfinished(self):
    # A call resumed and immediately interrupted again on the same line ends
    # the prior interval and begins a new one, so a syscall interrupted
    # twice produces two consecutive slices rather than one.
    return DiffTestBlueprint(
        trace=Path('resumed_then_unfinished.strace'),
        query="""
        SELECT ts, dur, name
        FROM slice
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","name"
        1700000052321000000,500000,"epoll_wait"
        1700000052321500000,500000,"epoll_wait"
        """))
