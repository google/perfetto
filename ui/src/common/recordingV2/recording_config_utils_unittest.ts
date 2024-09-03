// Copyright (C) 2023 The Android Open Source Project
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

import {createEmptyRecordConfig} from '../../controller/record_config_types';
import {genTraceConfig} from './recording_config_utils';
import {AndroidTargetInfo} from './recording_interfaces_v2';

test('genTraceConfig() can run without manipulating the input config', () => {
  const config = createEmptyRecordConfig();
  config.cpuSched = true; // Exercise ftrace

  const targetInfo: AndroidTargetInfo = {
    name: 'test',
    targetType: 'ANDROID',
    androidApiLevel: 31, // >= 32 to exercise symbolizeKsyms
    dataSources: [],
  };

  Object.freeze(config);
  const actual = genTraceConfig(config, targetInfo);

  const expected = {
    buffers: [
      {
        sizeKb: 63488,
        fillPolicy: 'DISCARD',
      },
      {
        sizeKb: 2048,
        fillPolicy: 'DISCARD',
      },
    ],
    dataSources: [
      {
        config: {
          name: 'android.packages_list',
          targetBuffer: 1,
        },
      },
      {
        config: {
          name: 'linux.system_info',
          targetBuffer: 1,
        },
      },
      {
        config: {
          name: 'linux.process_stats',
          targetBuffer: 1,
          processStatsConfig: {
            scanAllProcessesOnStart: true,
          },
        },
      },
      {
        config: {
          name: 'linux.ftrace',
          ftraceConfig: {
            ftraceEvents: [
              'sched/sched_switch',
              'power/suspend_resume',
              'sched/sched_wakeup',
              'sched/sched_wakeup_new',
              'sched/sched_waking',
              'sched/sched_process_exit',
              'sched/sched_process_free',
              'task/task_newtask',
              'task/task_rename',
              'sched/sched_blocked_reason',
            ],
            compactSched: {
              enabled: true,
            },
            symbolizeKsyms: true,
          },
        },
      },
    ],
    durationMs: 10000,
  };

  // Compare stringified versions to void issues with JS objects.
  expect(JSON.stringify(actual)).toEqual(JSON.stringify(expected));
});
