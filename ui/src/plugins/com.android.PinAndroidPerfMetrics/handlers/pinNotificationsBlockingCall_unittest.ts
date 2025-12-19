// Copyright (C) 2025 The Android Open Source Project
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

import {NotificationsBlockingCallMetricData} from './metricUtils';
import {pinNotificationsBlockingCallHandlerInstance} from './pinNotificationsBlockingCall';

const validMetricsTest: {
  inputMetric: string;
  expectedOutput: NotificationsBlockingCallMetricData;
}[] = [
  {
    inputMetric:
      'perfetto_android_notifications_blocking_call-blocking_calls-name-NotificationStackScrollLayout#onMeasure-total_dur_ms-mean',
    expectedOutput: {
      notificationName: 'NotificationStackScrollLayout#onMeasure',
      aggregation: 'total_dur_ms-mean',
    },
  },
  {
    inputMetric:
      'perfetto_android_notifications_blocking_call-blocking_calls-name-NotificationToplineView#onMeasure-cnt',
    expectedOutput: {
      notificationName: 'NotificationToplineView#onMeasure',
      aggregation: 'cnt',
    },
  },
  {
    inputMetric:
      'perfetto_android_notifications_blocking_call-blocking_calls-name-ExpNotRow#onNotifUpdated (leaf)-total_dur_ns',
    expectedOutput: {
      notificationName: 'ExpNotRow#onNotifUpdated (leaf)',
      aggregation: 'total_dur_ns',
    },
  },
  {
    inputMetric:
      'perfetto_android_notifications_blocking_call-blocking_calls-name-NotificationShadeWindowView#onMeasure-total_dur_ns-mean',
    expectedOutput: {
      notificationName: 'NotificationShadeWindowView#onMeasure',
      aggregation: 'total_dur_ns-mean',
    },
  },
  {
    inputMetric:
      'perfetto_android_notifications_blocking_call-blocking_calls-name-ImageFloatingTextView#onMeasure-total_dur_ns-mean',
    expectedOutput: {
      notificationName: 'ImageFloatingTextView#onMeasure',
      aggregation: 'total_dur_ns-mean',
    },
  },
];

const invalidMetricsTest: string[] = [
  'perfetto_android_blocking_call-cuj-name-com.google.android.apps.nexuslauncher-name-TASKBAR_EXPAND-blocking_calls-name-animation-total_dur_ms-mean',
  'perfetto_cuj_launcher-RECENTS_SCROLLING-counter_metrics-missed_sf_frames-mean',
];

const tester = pinNotificationsBlockingCallHandlerInstance;

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
