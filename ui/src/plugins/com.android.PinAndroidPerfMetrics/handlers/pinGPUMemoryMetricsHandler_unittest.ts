// Copyright (C) 2026 The Android Open Source Project
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

import type {ProcessMetricData} from './metricUtils';
import {pinGPUMemoryMetricsInstance} from './pinGPUMemoryMetricsHandler';

const validMetricsTest: {
  inputMetric: string;
  expectedOutput: ProcessMetricData;
}[] = [
  {
    inputMetric: 'perfetto_android_gpu-com.android.systemui-mem_max-max',
    expectedOutput: {
      process: 'com.android.systemui',
    },
  },
  {
    inputMetric: 'perfetto_android_gpu-systemui-mem_max-max',
    expectedOutput: {
      process: 'com.android.systemui',
    },
  },
  {
    inputMetric:
      'perfetto_android_gpu-com.google.android.apps.nexuslauncher-mem_max-max',
    expectedOutput: {
      process: 'com.google.android.apps.nexuslauncher',
    },
  },
  {
    inputMetric: 'perfetto_android_gpu-/system/bin/surfaceflinger-mem_avg-mean',
    expectedOutput: {
      process: '/system/bin/surfaceflinger',
    },
  },
];

const invalidMetricsTest: string[] = [
  'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
  'perfetto_android_bitmap_metric_max_val-com.android.systemui',
  'perfetto_ft_launcher-missed_sf_frames-mean',
  'perfetto_android_gpu-/vendor/bin/hw/surfaceflinger-mem_avg-mean',
  'perfetto_android_gpu-/system/bin/otherprocess-mem_avg-mean',
];

const tester = pinGPUMemoryMetricsInstance;

describe('PinGPUMemoryMetricsHandler_match', () => {
  it('parses metrics and returns expected data', () => {
    for (const testCase of validMetricsTest) {
      const parsedData = tester.match(testCase.inputMetric);
      expect(parsedData).toBeDefined();
      if (parsedData) {
        expect(parsedData).toEqual(testCase.expectedOutput);
      }
    }
  });
  it('parses metrics and returns undefined', () => {
    for (const testCase of invalidMetricsTest) {
      const parsedData = tester.match(testCase);
      expect(parsedData).toBeUndefined();
    }
  });
});
