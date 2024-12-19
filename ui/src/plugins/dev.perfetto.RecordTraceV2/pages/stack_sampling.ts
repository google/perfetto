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

import {splitLinesNonEmpty} from '../../../base/string_utils';
import protos from '../../../protos';
import {RecordProbe, RecordSubpage} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';
import {Slider} from './widgets/slider';
import {Textarea} from './widgets/textarea';

export function stackSamplingRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'stack_sampling',
    title: 'Stack sampling',
    subtitle: 'Lightweight cpu profiling',
    icon: 'full_stacked_bar_chart',
    probes: [tracedPerf()],
  };
}

function tracedPerf(): RecordProbe {
  const settings = {
    samplingFreq: new Slider({
      title: 'Sampling frequency',
      cssClass: '.thin',
      default: 100,
      values: [1, 10, 50, 100, 250, 500, 1000],
      unit: 'Hz',
    }),
    procs: new Textarea({
      placeholder:
        'Filters for processes to profile, one per line e.g.' +
        'com.android.phone\nlmkd\ncom.android.webview:sandboxed_process*',
    }),
  };
  return {
    id: 'traced_perf',
    title: 'Callstack sampling',
    image: 'rec_profiling.png',
    description:
      'Periodically records the current callstack (chain of ' +
      'function calls) of processes.',
    supportedPlatforms: ['ANDROID', 'LINUX'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const s = settings;
      const pkgs = splitLinesNonEmpty(s.procs.text);
      tc.addDataSource('linux.perf').perfEventConfig = {
        timebase: {
          frequency: s.samplingFreq.value,
          timestampClock: protos.PerfEvents.PerfClock.PERF_CLOCK_MONOTONIC,
        },
        callstackSampling: {
          scope:
            pkgs.length > 0
              ? {
                  targetCmdline: pkgs,
                }
              : undefined,
        },
      };
    },
  };
}
