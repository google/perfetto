#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

# hw_queue_id is the index in the list. Add a dummy for id 0.
trace.add_gpu_render_stages_hw_queue_spec([
    {
        'name': 'dummy queue 0'
    },
    {
        'name': 'queue 1',
        'description': 'queue desc 1'
    },
])

# stage_id is the index in the list. Add a dummy for id 0.
trace.add_gpu_render_stages_stage_spec([
    {
        'name': 'dummy stage 0'
    },
    {
        'name': 'stage 1'
    },
])

# Add two overlapping render stages on the same queue.
# hw_queue_id=1 and stage_id=1 should pick up the specs above.
trace.add_gpu_render_stages(
    ts=100, event_id=1, duration=10, hw_queue_id=1, stage_id=1, context=42)

trace.add_gpu_render_stages(
    ts=105, event_id=2, duration=10, hw_queue_id=1, stage_id=1, context=42)

sys.stdout.buffer.write(trace.trace.SerializeToString())
