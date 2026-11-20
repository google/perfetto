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

# Synthetic trace where all CUJ blocking calls sleep 100% off-CPU while Linux
# perf `instructions` counter samples cover the CUJ window. Verifies that
# `_android_cuj_bc_perf_instruction_intervals` stays non-empty and the tables
# emit `0.0` instructions rather than dropping the CUJ rows.

import sys
import synth_common

SYSUI_PID = 5000
SYSUI_RTID = 1555
LAYER_1 = "TX - first_layer#0"

SW_CONTEXT_SWITCHES = 4
HW_INSTRUCTIONS = 11
PERF_SEQ_ID = 4

trace = synth_common.create_trace()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)
trace.add_packet()
trace.add_process(
    pid=SYSUI_PID, ppid=SYSUI_PID, cmdline="com.android.systemui", uid=10001)
trace.add_thread(
    tid=SYSUI_PID, tgid=SYSUI_PID, cmdline="MainThread", name="MainThread")
trace.add_thread(
    tid=SYSUI_RTID, tgid=SYSUI_PID, cmdline="RenderThread", name="RenderThread")

trace.add_ftrace_packet(cpu=0)
cuj = "J<OFF_CPU_CUJ>"
trace.add_async_atrace_for_thread(
    ts=25_000_000, ts_end=50_000_000, buf=cuj, pid=SYSUI_PID, tid=SYSUI_PID)
trace.add_atrace_instant(
    ts=25_000_001, buf=cuj + "#UIThread", pid=SYSUI_PID, tid=SYSUI_PID)
trace.add_atrace_instant_for_track(
    ts=25_000_002,
    buf="FT#beginVsync#20",
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    track_name=cuj)
trace.add_atrace_instant_for_track(
    ts=25_000_010,
    buf="FT#layerId#0",
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    track_name=cuj)
trace.add_atrace_instant_for_track(
    ts=49_000_000,
    buf="FT#endVsync#25",
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    track_name=cuj)

# No sched events for SYSUI_PID during [26ms, 33ms]: blocking call is 100% off-CPU.
trace.add_frame(
    vsync=20,
    ts_do_frame=26_000_000,
    ts_end_do_frame=32_000_000,
    tid=SYSUI_PID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=28_000_000,
    ts_end=30_000_000,
    buf="binder transaction",
    tid=SYSUI_PID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=28_000_000,
    ts_end=30_000_000,
    buf="DrawFrames 20",
    tid=SYSUI_RTID,
    pid=SYSUI_PID)

trace.add_expected_surface_frame_start_event(
    ts=26_000_000,
    cookie=100020,
    token=20,
    display_frame_token=120,
    pid=SYSUI_PID,
    layer_name="")
trace.add_frame_end_event(ts=42_000_000, cookie=100020)
trace.add_actual_surface_frame_start_event(
    ts=26_000_000,
    cookie=200020,
    token=20,
    display_frame_token=120,
    pid=SYSUI_PID,
    present_type=1,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=1,
    prediction_type=3,
    layer_name=LAYER_1)
trace.add_frame_end_event(ts=33_000_000, cookie=200020)

# Perf instruction samples covering [20ms, 60ms] on CPU 0.
pkt = trace.add_packet(ts=0)
pkt.first_packet_on_sequence = True
pkt.sequence_flags = 1
pkt.trusted_packet_sequence_id = PERF_SEQ_ID
perf_defaults = pkt.trace_packet_defaults.perf_sample_defaults
perf_defaults.timebase.period = 1
perf_defaults.timebase.counter = SW_CONTEXT_SWITCHES
follower = perf_defaults.followers.add()
follower.counter = HW_INSTRUCTIONS

for ts, instructions in ((20_000_000, 0), (60_000_000, 40_000)):
  pkt = trace.add_packet(ts=ts)
  pkt.sequence_flags = 2
  pkt.trusted_packet_sequence_id = PERF_SEQ_ID
  pkt.perf_sample.cpu = 0
  pkt.perf_sample.pid = SYSUI_PID
  pkt.perf_sample.tid = SYSUI_PID
  pkt.perf_sample.timebase_count = ts
  pkt.perf_sample.follower_counts.append(instructions)

sys.stdout.buffer.write(trace.trace.SerializeToString())
