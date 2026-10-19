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

import type {CujMetricData} from './metricUtils';
import {pinLatencyCujInstance} from './pinLatencyCujMetricHandler';

const validMetricsTest: {
  inputMetric: string;
  expectedOutput: CujMetricData;
}[] = [
  {
    inputMetric: 'latency_ACTION_SWITCH_DISPLAY_UNFOLD-mean',
    expectedOutput: {cujName: 'ACTION_SWITCH_DISPLAY_UNFOLD'},
  },
  {
    inputMetric: 'latency_ACTION_FOLD_TO_AOD-max',
    expectedOutput: {cujName: 'ACTION_FOLD_TO_AOD'},
  },
  {
    inputMetric: 'latency_ACTION_CHECK_CREDENTIAL_UNLOCKED-mean',
    expectedOutput: {cujName: 'ACTION_CHECK_CREDENTIAL_UNLOCKED'},
  },
  {
    inputMetric: 'latency_ACTION_SWITCH_DISPLAY_UNFOLD.median',
    expectedOutput: {cujName: 'ACTION_SWITCH_DISPLAY_UNFOLD'},
  },
  {
    inputMetric: 'latency_ACTION_EXPAND_PANEL',
    expectedOutput: {cujName: 'ACTION_EXPAND_PANEL'},
  },
];

const invalidMetricsTest: string[] = [
  'perfetto_android_blocking_call-cuj-name-ACTION_SWITCH_DISPLAY_UNFOLD-blocking_calls-name-measure-total_dur_ms-mean',
  'perfetto_cuj_launcher-RECENTS_SCROLLING-counter_metrics-missed_sf_frames-mean',
  'latency_SHADE_EXPAND-mean',
];

const tester = pinLatencyCujInstance;

describe('testMetricParser_match', () => {
  it('parses metrics and returns expected data', () => {
    for (const testCase of validMetricsTest) {
      const parsedData = tester.match(testCase.inputMetric);
      // without this explicit check, undefined also passes the test
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
