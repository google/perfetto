#!/usr/bin/python
# Copyright (C) 2020 The Android Open Source Project
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

# Since we do various time based conversions to build cycles/sec, ensure that
# the timestamps look a bit realistic so they don't make those results look
# weird.
SEC = 1000000000

trace = synth_common.create_trace()

trace.add_system_info(arch='x86_64')
trace.packet.system_info.hz = 1

trace.add_packet(1)
trace.add_cpu([100, 200])
trace.add_cpu([1000, 2000])

trace.add_packet(1 * SEC)
trace.add_process_stats(pid=1, freqs={1: 1, 2: 1, 3: 1, 4: 1})
trace.add_process_stats(pid=2, freqs={1: 1, 2: 1, 3: 1, 4: 1})

trace.add_packet(2 * SEC)
trace.add_process_stats(pid=1, freqs={1: 2, 3: 2})
# Don't log anything for pid=2 thread, test that the packet at t=3 is based
# against t=2 anyway.

trace.add_packet(3 * SEC)
trace.add_process_stats(pid=1, freqs={2: 11, 4: 11})
trace.add_process_stats(pid=2, freqs={1: 11, 3: 11})

print(trace.trace.SerializeToString())
