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

import {CujScopedMetricData} from './metricUtils';
import {pinCujScopedJankInstance} from './pinCujScoped';

const validMetricsTest: {
  inputMetric: string;
  expectedOutput: CujScopedMetricData;
}[] = [
  {
    inputMetric:
      'perfetto_cuj_systemui-NOTIFICATION_SHADE_EXPAND_COLLAPSE::Expand-timeline_metrics-missed_app_frames-mean',
    expectedOutput: {
      process: 'com.android.systemui',
      cujName: 'NOTIFICATION_SHADE_EXPAND_COLLAPSE::Expand',
      jankType: 'app_frames',
    },
  },
  {
    inputMetric:
      'perfetto_cuj_systemui-SHADE_DIALOG_OPEN::internet-timeline_metrics-missed_sf_frames-mean',
    expectedOutput: {
      process: 'com.android.systemui',
      cujName: 'SHADE_DIALOG_OPEN::internet',
      jankType: 'sf_frames',
    },
  },
  {
    inputMetric:
      'perfetto_cuj_launcher-RECENTS_SCROLLING-counter_metrics-missed_sf_frames-mean',
    expectedOutput: {
      process: 'com.google.android.apps.nexuslauncher',
      cujName: 'RECENTS_SCROLLING',
      jankType: 'sf_frames',
    },
  },
];

const invalidMetricsTest: string[] = [
  'perfetto_ft_launcher-missed_sf_frames-mean',
  'perfetto_android_blocking_call-cuj-name-com.google.android.apps.nexuslauncher-name-TASKBAR_EXPAND-blocking_calls-name-animation-total_dur_ms-mean',
];

const tester = pinCujScopedJankInstance;

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
