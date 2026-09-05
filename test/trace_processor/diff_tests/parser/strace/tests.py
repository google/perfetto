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
    # Each line's leading `-f` pid becomes the slice's thread: the
    # "1234 <ts> write(...)" line lands on a different thread than the
    # pid-1000 syscalls in the same trace.
    return DiffTestBlueprint(
        trace=Path('basic.strace'),
        query="""
        SELECT s.name, t.tid
        FROM slice s
        JOIN thread_track tt ON s.track_id = tt.id
        JOIN thread t USING (utid)
        WHERE s.name IN ('close', 'write')
        ORDER BY s.ts;
        """,
        out=Csv("""
        "name","tid"
        "close",1000
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

  def test_strace_missing_pid_rejected(self):
    # missing_pid.strace mixes `-f` lines (leading pid) with two pid-less
    # syscall lines, as produced by strace without `-f`. Pid-less lines
    # can't be attributed to a thread that survives trace merging, so they
    # are skipped under a dedicated actionable stat while the `-f` lines
    # still import.
    return DiffTestBlueprint(
        trace=Path('missing_pid.strace'),
        query="""
        SELECT
          (SELECT COUNT(*) FROM slice) AS slices,
          (SELECT value FROM stats
            WHERE name = 'strace_missing_pid') AS missing_pid;
        """,
        out=Csv("""
        "slices","missing_pid"
        2,2
        """))

  def test_strace_bracketed_pid_and_durations(self):
    # Lines taken from real strace 6.13 `-ttt -T -f` stderr output, where
    # pids come as "[pid    75]" prefixes rather than the bare form strace
    # uses with -o, and every completed call carries a "<seconds>" duration
    # from -T. The root process's lines carry the prefix too, as they do
    # when strace is attached to several processes (-p A -p B).
    return DiffTestBlueprint(
        trace=Path('pid_prefix_durations.strace'),
        query="""
        SELECT s.ts, s.dur, s.name, t.tid
        FROM slice s
        JOIN thread_track tt ON s.track_id = tt.id
        JOIN thread t USING (utid)
        ORDER BY s.ts;
        """,
        out=Csv("""
        "ts","dur","name","tid"
        1787734451075961000,1261000,"execve",74
        1787734451084425000,371000,"execve",75
        1787734451084774000,310585000,"wait4",74
        1787734451088553000,305284000,"clock_nanosleep",75
        1787734451396054000,27000,"wait4",74
        """))

  def test_strace_duration_stripped_from_return_value(self):
    # The "<0.000027>" suffix belongs to the slice duration, not to the
    # recorded return value.
    return DiffTestBlueprint(
        trace=Path('pid_prefix_durations.strace'),
        query="""
        SELECT a.string_value
        FROM slice s
        JOIN args a ON s.arg_set_id = a.arg_set_id
        WHERE s.name = 'wait4' AND a.key = 'ret'
        ORDER BY s.ts;
        """,
        out=Csv("""
        "string_value"
        "75"
        "-1 ECHILD (No child processes)"
        """))

  def test_strace_bracketed_pid_non_syscall_lines(self):
    # strace's own "Process N attached" diagnostic, the exit banner and the
    # signal-delivery line are all skipped as ordinary non-syscall lines.
    # In particular the ':' in "strace:" must not be read as a `-t`/`-tt`
    # timestamp: that would raise a spurious "re-run with -ttt" error on a
    # trace that already uses -ttt.
    return DiffTestBlueprint(
        trace=Path('pid_prefix_durations.strace'),
        query="""
        SELECT name, value
        FROM stats
        WHERE name IN ('strace_parse_failure',
                        'strace_unsupported_timestamp_format',
                        'strace_missing_pid')
        ORDER BY name;
        """,
        out=Csv("""
        "name","value"
        "strace_missing_pid",0
        "strace_parse_failure",3
        "strace_unsupported_timestamp_format",0
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
