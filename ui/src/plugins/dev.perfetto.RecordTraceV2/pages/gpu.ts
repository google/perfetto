// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {RecordProbe, RecordSubpage} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';

export function gpuRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'gpu',
    title: 'GPU',
    subtitle: 'GPU Frequency, memory',
    icon: 'aspect_ratio',
    probes: [
      gpuFreq(),
      gpuMemory(),
      gpuWorkPeriod(),
      gpuRenderStages(),
      gpuMaliCounters(),
      gpuMaliFenceEvents(),
    ],
  };
}

function gpuFreq(): RecordProbe {
  return {
    id: 'gpu_frequency',
    image: 'rec_cpu_freq.png',
    title: 'GPU frequency',
    description: 'Records gpu frequency via ftrace',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addFtraceEvents('power/gpu_frequency');
    },
  };
}

function gpuMemory(): RecordProbe {
  return {
    id: 'gpu_memory',
    image: 'rec_gpu_mem_total.png',
    title: 'GPU memory',
    description:
      'Allows to track per process and global total GPU memory usages. ' +
      '(Available on recent Android 12+ kernels)',
    supportedPlatforms: ['ANDROID'],
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addDataSource('android.gpu.memory');
      tc.addFtraceEvents('gpu_mem/gpu_mem_total');
    },
  };
}

function gpuWorkPeriod(): RecordProbe {
  return {
    id: 'gpu_work_period',
    title: 'GPU work period',
    description:
      'Allows to track per package GPU work.' +
      '(Available on recent Android 14+ kernels)',
    supportedPlatforms: ['ANDROID'],
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addFtraceEvents('power/gpu_work_period');
    },
  };
}

function gpuRenderStages(): RecordProbe {
  return {
    id: 'gpu_renderstages',
    title: 'GPU Render Stages',
    description:
      'Records GPU render stage events. To check if your device supports this feature run:' +
      '```adb shell getprop graphics.gpu.profiler.support```' +
      'To enable the event producer, run:' +
      '```adb shell setprop debug.graphics.gpu.profiler.perfetto 1```',
    supportedPlatforms: ['ANDROID'],
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addDataSource('gpu.renderstages', 'default');
    },
  };
}

function gpuMaliCounters(): RecordProbe {
  return {
    id: 'gpu_mali_counters',
    title: 'Mali GPU Counters',
    description:
      'Records Mali GPU performance counters (Available on Valhall+).' +
      'To enable the event producer, run: ```adb shell gpu_counter_producer```',
    supportedPlatforms: ['ANDROID'],
    genConfig: function (tc: TraceConfigBuilder) {
      const cfg = tc.addDataSource('gpu.counters', 'default');
      cfg.gpuCounterConfig = {
        // Update 10 times per second
        counterPeriodNs: 100000,
        // All Mali counters
        counterIds: [
          1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
          21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
          38, 39, 40, 41, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 58,
          59, 60, 82, 83, 84, 85, 86, 87, 88, 94, 95, 96, 97, 98, 99, 100, 101,
          102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
          116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
          130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
          144, 145, 146, 147, 148, 151, 152, 153, 154, 156, 157, 159, 160, 161,
          162, 163, 164, 166, 167, 168, 170, 172, 173, 174, 175, 176, 177, 178,
          179, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193,
          196, 198, 199, 200, 201, 202, 203, 204, 205, 207, 210, 211, 212, 213,
          214, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228,
          229, 230, 231, 232, 235, 236, 241, 242, 243, 244, 245, 246, 247, 253,
          257, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276, 277,
          278, 279, 280, 281, 282, 283, 285, 286, 287, 288, 289, 290, 291, 292,
          293, 294, 295, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307,
          308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319, 320, 321,
          323, 324, 347, 348, 349, 461, 544, 65535, 65536, 65538, 65541, 65542,
          65543, 65544, 65545, 65546, 65547, 65548, 65549, 65550, 65551, 65552,
          65553, 65554, 65555, 65556, 65557, 65560, 65562, 65565, 65566, 65567,
          65569, 65570, 65572, 65575, 65577, 65578, 65579, 65580, 65581, 65582,
          65583, 65584, 65585, 65586, 65588, 65589, 65591, 65593, 65594, 65596,
          65597, 65598, 65599, 65601, 65602, 65603, 65609, 65618, 65619, 65620,
          65626, 65627, 65628, 65629, 65630, 65631, 65632, 65633, 65634, 65635,
          65636, 65637, 65638, 65639, 65641, 65642, 65643, 65644, 65645, 65646,
          65653, 65654, 65655, 65658, 65659, 65660, 65664, 65665, 65666, 65667,
          65668, 65671, 65672, 65673,
        ],
      };
    },
  };
}

function gpuMaliFenceEvents(): RecordProbe {
  return {
    id: 'gpu_mali_fence_events',
    title: 'Mali Fence Events',
    description: 'Records Mali fence events (Available on Valhall+).',
    supportedPlatforms: ['ANDROID'],
    genConfig: function (tc: TraceConfigBuilder) {
      const cfg = tc.addDataSource('linux.ftrace', 'default');
      cfg.ftraceConfig = {
        ftraceEvents: [
          'mali/mali_KCPU_FENCE_SIGNAL',
          'mali/mali_KCPU_FENCE_WAIT_END',
          'mali/mali_KCPU_FENCE_WAIT_START',
        ],
      };
    },
  };
}
