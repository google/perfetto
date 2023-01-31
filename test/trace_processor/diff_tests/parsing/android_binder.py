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

PROCESS_A_NAME = 'test_process_a'
PROCESS_B_NAME = 'test_process_b'
PROCESS_C_NAME = 'test_process_c'
PROCESS_A_PID = 1
PROCESS_B_PID = 2
PROCESS_C_PID = 3
PROCESS_A_PPID = 4
PROCESS_B_PPID = 5
PROCESS_C_PPID = 6
PROCESS_A_TID = 7
# These values need to be the same to keep track of process ids in kernel space
PROCESS_B_TID = PROCESS_B_PID
PROCESS_C_TID = PROCESS_C_PID

trace = synth_common.create_trace()
trace.add_packet()
trace.add_process(PROCESS_A_PID, PROCESS_A_PPID, PROCESS_A_NAME)
trace.add_process(PROCESS_B_PID, PROCESS_B_PPID, PROCESS_B_NAME)
trace.add_process(PROCESS_C_PID, PROCESS_C_PPID, PROCESS_C_NAME)
trace.add_thread(PROCESS_A_TID, PROCESS_A_PID, cmdline='Binder')
trace.add_ftrace_packet(cpu=0)

trace.add_binder_transaction(
    transaction_id=1,
    ts_start=1,
    ts_end=2,
    tid=PROCESS_A_TID,
    pid=PROCESS_A_PID,
    reply_id=2,
    reply_ts_start=3,
    reply_ts_end=4,
    reply_tid=PROCESS_B_TID,
    reply_pid=PROCESS_B_PID)
trace.add_binder_transaction(
    transaction_id=3,
    ts_start=5,
    ts_end=6,
    tid=PROCESS_A_TID,
    pid=PROCESS_A_PID,
    reply_id=4,
    reply_ts_start=7,
    reply_ts_end=8,
    reply_tid=PROCESS_C_TID,
    reply_pid=PROCESS_C_PID)

sys.stdout.buffer.write(trace.trace.SerializeToString())
