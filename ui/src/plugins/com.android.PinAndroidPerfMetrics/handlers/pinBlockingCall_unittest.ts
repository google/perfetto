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
import {translateBlockingCall} from './pinBlockingCall';

const validMetricsTest: {
  inputMetric: string;
  expectedOutput: PinRequest[];
}[] = [
  {
    inputMetric:
      'perfetto_android_blocking_call-cuj-name-com.google.android.apps.nexuslauncher-name-TASKBAR_EXPAND-blocking_calls-name-animation-total_dur_ms-mean',
    expectedOutput: [
      {
        type: PinRequestType.BlockingCall,
        process: 'com.google.android.apps.nexuslauncher',
        cujName: 'TASKBAR_EXPAND',
        blockingCallName: 'animation',
        aggregation: 'total_dur_ms-mean',
      },
    ],
  },

  {
    inputMetric:
      'perfetto_android_blocking_call-cuj-name-com.android.systemui-name-NOTIFICATION_SHADE_EXPAND_COLLAPSE::Collapse-blocking_calls-name-AIDL::java::IPackageManager::isProtectedBroadcast::server-cnt-mean',
    expectedOutput: [
      {
        type: PinRequestType.BlockingCall,
        process: 'com.android.systemui',
        cujName: 'NOTIFICATION_SHADE_EXPAND_COLLAPSE::Collapse',
        blockingCallName:
          'AIDL::java::IPackageManager::isProtectedBroadcast::server',
        aggregation: 'cnt-mean',
      },
    ],
  },
  {
    inputMetric:
      'perfetto_android_blocking_call-cuj-name-com.android.systemui-name-SPLASHSCREEN_EXIT_ANIM-blocking_calls-name-AIDL::java::IPackageManager::isProtectedBroadcast::server-cnt-mean',
    expectedOutput: [
      {
        type: PinRequestType.BlockingCall,
        process: 'com.android.systemui',
        cujName: 'SPLASHSCREEN_EXIT_ANIM',
        blockingCallName:
          'AIDL::java::IPackageManager::isProtectedBroadcast::server',
        aggregation: 'cnt-mean',
      },
    ],
  },
  // test cases for blocking call per-frame metrics.
  {
    inputMetric:
      'perfetto_android_blocking_call_per_frame-cuj-name-com.android.systemui-name-LOCKSCREEN_TRANSITION_FROM_AOD::DEFAULT-blocking_calls-name-Handler: android.widget.DateTimeView-mean_cnt_per_frame-mean',
    expectedOutput: [
      {
        type: PinRequestType.BlockingCall,
        process: 'com.android.systemui',
        cujName: 'LOCKSCREEN_TRANSITION_FROM_AOD::DEFAULT',
        blockingCallName: 'Handler: android.widget.DateTimeView',
        aggregation: 'mean_cnt_per_frame-mean',
      },
    ],
  },
  {
    inputMetric:
      'perfetto_android_blocking_call_per_frame-cuj-name-com.google.android.apps.nexuslauncher-name-TASKBAR_EXPAND::Manually unstashed-blocking_calls-name-Handler: android.view.View-max_dur_per_frame_ns-mean',
    expectedOutput: [
      {
        type: PinRequestType.BlockingCall,
        process: 'com.google.android.apps.nexuslauncher',
        cujName: 'TASKBAR_EXPAND::Manually unstashed',
        blockingCallName: 'Handler: android.view.View',
        aggregation: 'max_dur_per_frame_ns-mean',
      },
    ],
  },
  {
    inputMetric:
      'perfetto_android_blocking_call_per_frame-cuj-name-com.android.systemui-name-NOTIFICATION_SHADE_EXPAND_COLLAPSE::Collapse-blocking_calls-name-input-mean_dur_per_frame_ns-max',
    expectedOutput: [
      {
        type: PinRequestType.BlockingCall,
        process: 'com.android.systemui',
        cujName: 'NOTIFICATION_SHADE_EXPAND_COLLAPSE::Collapse',
        blockingCallName: 'input',
        aggregation: 'mean_dur_per_frame_ns-max',
      },
    ],
  },
];

const invalidMetricsTest: string[] = [
  'perfetto_ft_launcher-missed_sf_frames-mean',
  'perfetto_cuj_launcher-RECENTS_SCROLLING-counter_metrics-missed_sf_frames-mean',
];

describe('testMetricParser_match', () => {
  it('parses metrics and returns expected data', () => {
    for (const testCase of validMetricsTest) {
      const parsedData = translateBlockingCall(testCase.inputMetric);
      expect(parsedData).toEqual(testCase.expectedOutput);
    }
  });
  it('parses metrics and returns empty array', () => {
    for (const testCase of invalidMetricsTest) {
      const parsedData = translateBlockingCall(testCase);
      expect(parsedData).toEqual([]);
    }
  });
});
