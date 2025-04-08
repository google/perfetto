#!/usr/bin/env python3
# Copyright (C) 2018 The Android Open Source Project
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
trace.add_battery_counters(100_000_000_000, 5500000, 0.2, 990000, 12, 8448000)
trace.add_battery_counters(200_000_000_000, 5490000, 0.8, 710000, 93, 8448000)
trace.add_battery_counters(300_000_000_000, 5480000, 0.5, 510000, 5, 8452000)
trace.add_battery_counters_no_curr_ua(400_000_000_000, 5470000, 0.3, 25,
                                      8460000)

sys.stdout.buffer.write(trace.trace.SerializeToString())
