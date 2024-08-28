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

import {BlockingCallMetricData} from './metricUtils';
import {pinBlockingCallHandlerInstance} from './pinBlockingCall';

const validMetricsTest: {
  inputMetric: string;
  expectedOutput: BlockingCallMetricData;
}[] = [
  {
    inputMetric:
      'perfetto_android_blocking_call-cuj-name-com.google.android.apps.nexuslauncher-name-TASKBAR_EXPAND-blocking_calls-name-animation-total_dur_ms-mean',
    expectedOutput: {
      process: 'com.google.android.apps.nexuslauncher',
      cujName: 'TASKBAR_EXPAND',
      blockingCallName: 'animation',
      aggregation: 'total_dur_ms-mean',
    },
  },

  {
    inputMetric:
      'perfetto_android_blocking_call-cuj-name-com.android.systemui-name-NOTIFICATION_SHADE_EXPAND_COLLAPSE::Collapse-blocking_calls-name-AIDL::java::IPackageManager::isProtectedBroadcast::server-cnt-mean',
    expectedOutput: {
      process: 'com.android.systemui',
      cujName: 'NOTIFICATION_SHADE_EXPAND_COLLAPSE::Collapse',
      blockingCallName:
        'AIDL::java::IPackageManager::isProtectedBroadcast::server',
      aggregation: 'cnt-mean',
    },
  },
  {
    inputMetric:
      'perfetto_android_blocking_call-cuj-name-com.android.systemui-name-SPLASHSCREEN_EXIT_ANIM-blocking_calls-name-AIDL::java::IPackageManager::isProtectedBroadcast::server-cnt-mean',
    expectedOutput: {
      process: 'com.android.systemui',
      cujName: 'SPLASHSCREEN_EXIT_ANIM',
      blockingCallName:
        'AIDL::java::IPackageManager::isProtectedBroadcast::server',
      aggregation: 'cnt-mean',
    },
  },
];

const invalidMetricsTest: string[] = [
  'perfetto_ft_launcher-missed_sf_frames-mean',
  'perfetto_cuj_launcher-RECENTS_SCROLLING-counter_metrics-missed_sf_frames-mean',
];

const tester = pinBlockingCallHandlerInstance;

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
