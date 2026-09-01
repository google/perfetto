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

from os import sys, path

import synth_common

# GpuCounterDescriptor.GpuCounterSpec.ValueDirection.VALUE_DIRECTION_FORWARDS_LOOKING
FORWARDS_LOOKING = 2

trace = synth_common.create_trace()

trace.add_gpu_counter_spec(
    ts=1,
    gpu_id=0,
    counter_id=1,
    name="forward_counter",
    value_direction=FORWARDS_LOOKING,
)

# A non-zero sample followed by a run of zeros (e.g. the producer marking the
# counter as inactive). With BACKWARDS_LOOKING this 100 would be back-shifted
# onto the previous timestamp; with FORWARDS_LOOKING the value at each ts is
# the value reported by the producer.
trace.add_gpu_counter(10, 1, 100)
trace.add_gpu_counter(20, 1, 0)
trace.add_gpu_counter(30, 1, 0)
trace.add_gpu_counter(40, 1, 50)

sys.stdout.buffer.write(trace.trace.SerializeToString())
