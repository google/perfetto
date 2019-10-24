#!/usr/bin/python
# Copyright (C) 2019 The Android Open Source Project
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
sys.path.append(path.dirname(path.dirname(path.abspath(__file__))))
import synth_common

trace = synth_common.create_trace()

# Chrome renderer processes don't know their "true" tids on some platforms.
# Instead, they each write tids that start at 1 - which means, the same tids are
# used in multiple different processes at the same time. This trace replicates
# such a situation.

trace.add_thread_track_descriptor(
    ps=1, ts=0, uuid=1, pid=5, tid=1, thread_name="t1", inc_state_cleared=True)
trace.add_thread_track_descriptor(
    ps=1, ts=0, uuid=2, pid=10, tid=1, thread_name="t2")

trace.add_track_event(
    ps=1, ts=1000, track_uuid=1, cat="cat", name="name1", type=3)
trace.add_track_event(
    ps=1, ts=2000, track_uuid=2, cat="cat", name="name2", type=3)

print(trace.trace.SerializeToString())
