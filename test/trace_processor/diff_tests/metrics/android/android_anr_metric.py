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

from os import sys
import synth_common

from google.protobuf import text_format

SS_PID = 1234

trace = synth_common.create_trace()

trace.add_packet()
trace.add_process(pid=SS_PID, ppid=1, cmdline="system_server", uid=10001)

# Add first ANR.
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=1000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.google.android.app1 11167#da24554c-452a-4ae1-b74a-fb898f6e0982",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=1001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId da24554c-452a-4ae1-b74a-fb898f6e0982):Test ANR subject 1",
    cnt=1)

# Add second ANR.
# Does not include PID
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=2000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.google.android.app2#8612fece-c2f1-4aeb-9d45-8e6d9d0201cf",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=2001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 8612fece-c2f1-4aeb-9d45-8e6d9d0201cf):Test ANR subject 2",
    cnt=1)

# Add third ANR.
# Does not include PID or subject
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=3000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.google.android.app3#c25916a0-a8f0-41f3-87df-319e06471a0f",
    cnt=1)

sys.stdout.buffer.write(trace.trace.SerializeToString())
