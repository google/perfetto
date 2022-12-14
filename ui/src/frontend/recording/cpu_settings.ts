// Copyright (C) 2022 The Android Open Source Project
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

import * as m from 'mithril';

import {Probe, ProbeAttrs, Slider, SliderAttrs} from '../record_widgets';
import {POLL_INTERVAL_MS, RecordingSectionAttrs} from './recording_sections';

export class CpuSettings implements m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    return m(
        `.record-section${attrs.cssClass}`,
        m(Probe,
          {
            title: 'Coarse CPU usage counter',
            img: 'rec_cpu_coarse.png',
            descr: `Lightweight polling of CPU usage counters via /proc/stat.
                    Allows to periodically monitor CPU usage.`,
            setEnabled: (cfg, val) => cfg.cpuCoarse = val,
            isEnabled: (cfg) => cfg.cpuCoarse,
          } as ProbeAttrs,
          m(Slider, {
            title: 'Poll interval',
            cssClass: '.thin',
            values: POLL_INTERVAL_MS,
            unit: 'ms',
            set: (cfg, val) => cfg.cpuCoarsePollMs = val,
            get: (cfg) => cfg.cpuCoarsePollMs,
          } as SliderAttrs)),
        m(Probe, {
          title: 'Scheduling details',
          img: 'rec_cpu_fine.png',
          descr: 'Enables high-detailed tracking of scheduling events',
          setEnabled: (cfg, val) => cfg.cpuSched = val,
          isEnabled: (cfg) => cfg.cpuSched,
        } as ProbeAttrs),
        m(Probe,
          {
            title: 'CPU frequency and idle states',
            img: 'rec_cpu_freq.png',
            descr:
                'Records cpu frequency and idle state changes via ftrace and sysfs',
            setEnabled: (cfg, val) => cfg.cpuFreq = val,
            isEnabled: (cfg) => cfg.cpuFreq,
          } as ProbeAttrs,
          m(Slider, {
            title: 'Sysfs poll interval',
            cssClass: '.thin',
            values: POLL_INTERVAL_MS,
            unit: 'ms',
            set: (cfg, val) => cfg.cpuFreqPollMs = val,
            get: (cfg) => cfg.cpuFreqPollMs,
          } as SliderAttrs)),
        m(Probe, {
          title: 'Syscalls',
          img: 'rec_syscalls.png',
          descr: `Tracks the enter and exit of all syscalls. On Android
                requires a userdebug or eng build.`,
          setEnabled: (cfg, val) => cfg.cpuSyscall = val,
          isEnabled: (cfg) => cfg.cpuSyscall,
        } as ProbeAttrs));
  }
}
