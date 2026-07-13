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
import {pinActivityOrBinderLeaksMetricsInstance} from './pinActivityOrBinderLeaksMetricsHandler';

const validMetricsTest: {
  inputMetric: string;
  expectedOutput: ProcessMetricData;
}[] = [
  {
    inputMetric: 'com.android.systemui_Activities-last-first-diff',
    expectedOutput: {
      process: 'com.android.systemui',
    },
  },
  {
    inputMetric: 'systemui_View-last-first-diff',
    expectedOutput: {
      process: 'com.android.systemui', // Should be expanded
    },
  },
  {
    inputMetric:
      'com.google.android.apps.nexuslauncher_ViewRootImpl-last-first-diff',
    expectedOutput: {
      process: 'com.google.android.apps.nexuslauncher',
    },
  },
  {
    inputMetric: 'com.android.systemui_Local Binders-last-first-diff',
    expectedOutput: {
      process: 'com.android.systemui',
    },
  },
  {
    inputMetric: 'com.android.systemui_Proxy Binders-last-first-diff',
    expectedOutput: {
      process: 'com.android.systemui',
    },
  },
];

const invalidMetricsTest: string[] = [
  'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
  'com.android.systemui_Activities-last-first',
  'com.android.systemui_Other-last-first-diff',
  'perfetto_ft_launcher-missed_sf_frames-mean',
];

const tester = pinActivityOrBinderLeaksMetricsInstance;

describe('PinActivityOrBinderLeaksMetricsHandler_match', () => {
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
