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

import m from 'mithril';
import {
  Dropdown,
  DropdownAttrs,
  Probe,
  ProbeAttrs,
  Slider,
  SliderAttrs,
  Textarea,
  TextareaAttrs,
  Toggle,
  ToggleAttrs,
} from '../record_widgets';
import {RecordingSectionAttrs} from './recording_sections';

const FTRACE_CATEGORIES = new Map<string, string>();
FTRACE_CATEGORIES.set('binder/*', 'binder');
FTRACE_CATEGORIES.set('block/*', 'block');
FTRACE_CATEGORIES.set('clk/*', 'clk');
FTRACE_CATEGORIES.set('ext4/*', 'ext4');
FTRACE_CATEGORIES.set('f2fs/*', 'f2fs');
FTRACE_CATEGORIES.set('i2c/*', 'i2c');
FTRACE_CATEGORIES.set('irq/*', 'irq');
FTRACE_CATEGORIES.set('kmem/*', 'kmem');
FTRACE_CATEGORIES.set('memory_bus/*', 'memory_bus');
FTRACE_CATEGORIES.set('mmc/*', 'mmc');
FTRACE_CATEGORIES.set('oom/*', 'oom');
FTRACE_CATEGORIES.set('power/*', 'power');
FTRACE_CATEGORIES.set('regulator/*', 'regulator');
FTRACE_CATEGORIES.set('sched/*', 'sched');
FTRACE_CATEGORIES.set('sync/*', 'sync');
FTRACE_CATEGORIES.set('task/*', 'task');
FTRACE_CATEGORIES.set('task/*', 'task');
FTRACE_CATEGORIES.set('vmscan/*', 'vmscan');
FTRACE_CATEGORIES.set('fastrpc/*', 'fastrpc');

export class AdvancedSettings
  implements m.ClassComponent<RecordingSectionAttrs>
{
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    return m(
      `.record-section${attrs.cssClass}`,
      m(
        Probe,
        {
          title: 'Advanced ftrace config',
          img: 'rec_ftrace.png',
          descr: `Enable individual events and tune the kernel-tracing (ftrace)
                  module. The events enabled here are in addition to those from
                  enabled by other probes.`,
          setEnabled: (cfg, val) => (cfg.ftrace = val),
          isEnabled: (cfg) => cfg.ftrace,
        } as ProbeAttrs,
        m(Toggle, {
          title: 'Resolve kernel symbols',
          cssClass: '.thin',
          descr: `Enables lookup via /proc/kallsyms for workqueue,
              sched_blocked_reason and other events
              (userdebug/eng builds only).`,
          setEnabled: (cfg, val) => (cfg.symbolizeKsyms = val),
          isEnabled: (cfg) => cfg.symbolizeKsyms,
        } as ToggleAttrs),
        m(Slider, {
          title: 'Buf size',
          cssClass: '.thin',
          values: [0, 512, 1024, 2 * 1024, 4 * 1024, 16 * 1024, 32 * 1024],
          unit: 'KB',
          zeroIsDefault: true,
          set: (cfg, val) => (cfg.ftraceBufferSizeKb = val),
          get: (cfg) => cfg.ftraceBufferSizeKb,
        } as SliderAttrs),
        m(Slider, {
          title: 'Drain rate',
          cssClass: '.thin',
          values: [0, 100, 250, 500, 1000, 2500, 5000],
          unit: 'ms',
          zeroIsDefault: true,
          set: (cfg, val) => (cfg.ftraceDrainPeriodMs = val),
          get: (cfg) => cfg.ftraceDrainPeriodMs,
        } as SliderAttrs),
        m(Dropdown, {
          title: 'Event groups',
          cssClass: '.multicolumn.ftrace-events',
          options: FTRACE_CATEGORIES,
          set: (cfg, val) => (cfg.ftraceEvents = val),
          get: (cfg) => cfg.ftraceEvents,
        } as DropdownAttrs),
        m(Textarea, {
          placeholder:
            'Add extra events, one per line, e.g.:\n' +
            'sched/sched_switch\n' +
            'kmem/*',
          set: (cfg, val) => (cfg.ftraceExtraEvents = val),
          get: (cfg) => cfg.ftraceExtraEvents,
        } as TextareaAttrs),
      ),
    );
  }
}
