#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
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

from os import sys, path

import synth_common

trace = synth_common.create_trace()
# sys_write id is 64 on arm64
trace.add_system_info(arch="aarch64")

trace.add_ftrace_packet(cpu=0)
# expect: one normal sys_write slice
trace.add_sys_enter(ts=100, tid=42, id=64)
trace.add_sys_exit(ts=200, tid=42, id=64, ret=0)
# expect: truncated sys_write slice due to nesting workarounds
trace.add_sys_enter(ts=300, tid=42, id=64)
trace.add_sys_exit(ts=400, tid=42, id=64, ret=0)
# expect: truncated sys_write slice due to nesting workarounds
trace.add_sys_enter(ts=600, tid=42, id=64)
trace.add_sys_exit(ts=700, tid=42, id=64, ret=0)

trace.add_atrace_begin(ts=350, tid=42, pid=42, buf='test')
trace.add_atrace_end(ts=650, tid=42, pid=42)

sys.stdout.buffer.write(trace.trace.SerializeToString())
