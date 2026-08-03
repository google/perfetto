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

import type {PinRequest} from './pinRequest';
import {PinRequestType} from './pinRequest';
import {translateFullTraceJank} from './fullTraceJankMetricHandler';

const validMetricsTest: {
  inputMetric: string;
  expectedOutput: PinRequest[];
}[] = [
  {
    inputMetric: 'perfetto_ft_launcher-missed_app_frames-mean',
    expectedOutput: [
      {
        type: PinRequestType.FullTraceMissedFrames,
        process: 'com.google.android.apps.nexuslauncher',
        jankType: 'app_frames',
        isWeighted: false,
      },
    ],
  },
  {
    inputMetric: 'perfetto_ft_systemui-missed_sf_frames-mean',
    expectedOutput: [
      {
        type: PinRequestType.FullTraceMissedFrames,
        process: 'com.android.systemui',
        jankType: 'sf_frames',
        isWeighted: false,
      },
    ],
  },
  {
    inputMetric: 'perfetto_ft_systemui-missed_app_frames-mean',
    expectedOutput: [
      {
        type: PinRequestType.FullTraceMissedFrames,
        process: 'com.android.systemui',
        jankType: 'app_frames',
        isWeighted: false,
      },
    ],
  },
  {
    inputMetric: 'perfetto_ft_systemui-weighted_missed_app_frames-mean',
    expectedOutput: [
      {
        type: PinRequestType.FullTraceMissedFrames,
        process: 'com.android.systemui',
        jankType: 'app_frames',
        isWeighted: true,
      },
    ],
  },
];

const invalidMetricsTest: string[] = [
  'perfetto_cuj_launcher-RECENTS_SCROLLING-counter_metrics-missed_sf_frames-mean',
  'perfetto_android_blocking_call-cuj-name-com.google.android.apps.nexuslauncher-name-TASKBAR_EXPAND-blocking_calls-name-animation-total_dur_ms-mean',
];

describe('testMetricParser_match', () => {
  it('parses metrics and returns expected data', () => {
    for (const testCase of validMetricsTest) {
      const parsedData = translateFullTraceJank(testCase.inputMetric);
      expect(parsedData).toEqual(testCase.expectedOutput);
    }
  });
  it('parses metrics and returns empty array', () => {
    for (const testCase of invalidMetricsTest) {
      const parsedData = translateFullTraceJank(testCase);
      expect(parsedData).toEqual([]);
    }
  });
});
