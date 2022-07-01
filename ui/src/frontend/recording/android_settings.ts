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
import {globals} from '../globals';
import {
  Dropdown,
  DropdownAttrs,
  Probe,
  ProbeAttrs,
  Textarea,
  TextareaAttrs,
  Toggle,
  ToggleAttrs,
} from '../record_widgets';
import {RecordingSectionAttrs} from './recording_sections';

const LOG_BUFFERS = new Map<string, string>();
LOG_BUFFERS.set('LID_CRASH', 'Crash');
LOG_BUFFERS.set('LID_DEFAULT', 'Main');
LOG_BUFFERS.set('LID_EVENTS', 'Binary events');
LOG_BUFFERS.set('LID_KERNEL', 'Kernel');
LOG_BUFFERS.set('LID_RADIO', 'Radio');
LOG_BUFFERS.set('LID_SECURITY', 'Security');
LOG_BUFFERS.set('LID_STATS', 'Stats');
LOG_BUFFERS.set('LID_SYSTEM', 'System');

const ATRACE_CATEGORIES = new Map<string, string>();
ATRACE_CATEGORIES.set('adb', 'ADB');
ATRACE_CATEGORIES.set('aidl', 'AIDL calls');
ATRACE_CATEGORIES.set('am', 'Activity Manager');
ATRACE_CATEGORIES.set('audio', 'Audio');
ATRACE_CATEGORIES.set('binder_driver', 'Binder Kernel driver');
ATRACE_CATEGORIES.set('binder_lock', 'Binder global lock trace');
ATRACE_CATEGORIES.set('bionic', 'Bionic C library');
ATRACE_CATEGORIES.set('camera', 'Camera');
ATRACE_CATEGORIES.set('dalvik', 'ART & Dalvik');
ATRACE_CATEGORIES.set('database', 'Database');
ATRACE_CATEGORIES.set('gfx', 'Graphics');
ATRACE_CATEGORIES.set('hal', 'Hardware Modules');
ATRACE_CATEGORIES.set('input', 'Input');
ATRACE_CATEGORIES.set('network', 'Network');
ATRACE_CATEGORIES.set('nnapi', 'Neural Network API');
ATRACE_CATEGORIES.set('pm', 'Package Manager');
ATRACE_CATEGORIES.set('power', 'Power Management');
ATRACE_CATEGORIES.set('res', 'Resource Loading');
ATRACE_CATEGORIES.set('rro', 'Resource Overlay');
ATRACE_CATEGORIES.set('rs', 'RenderScript');
ATRACE_CATEGORIES.set('sm', 'Sync Manager');
ATRACE_CATEGORIES.set('ss', 'System Server');
ATRACE_CATEGORIES.set('vibrator', 'Vibrator');
ATRACE_CATEGORIES.set('video', 'Video');
ATRACE_CATEGORIES.set('view', 'View System');
ATRACE_CATEGORIES.set('webview', 'WebView');
ATRACE_CATEGORIES.set('wm', 'Window Manager');

class AtraceAppsList implements m.ClassComponent {
  view() {
    if (globals.state.recordConfig.allAtraceApps) {
      return m('div');
    }

    return m(Textarea, {
      placeholder: 'Apps to profile, one per line, e.g.:\n' +
          'com.android.phone\n' +
          'lmkd\n' +
          'com.android.nfc',
      cssClass: '.atrace-apps-list',
      set: (cfg, val) => cfg.atraceApps = val,
      get: (cfg) => cfg.atraceApps,
    } as TextareaAttrs);
  }
}

export class AndroidSettings implements
    m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    return m(
        `.record-section${attrs.cssClass}`,
        m(Probe,
          {
            title: 'Atrace userspace annotations',
            img: 'rec_atrace.png',
            descr: `Enables C++ / Java codebase annotations (ATRACE_BEGIN() /
                      os.Trace())`,
            setEnabled: (cfg, val) => cfg.atrace = val,
            isEnabled: (cfg) => cfg.atrace,
          } as ProbeAttrs,
          m(Dropdown, {
            title: 'Categories',
            cssClass: '.multicolumn.atrace-categories',
            options: ATRACE_CATEGORIES,
            set: (cfg, val) => cfg.atraceCats = val,
            get: (cfg) => cfg.atraceCats,
          } as DropdownAttrs),
          m(Toggle, {
            title: 'Record events from all Android apps and services',
            descr: '',
            setEnabled: (cfg, val) => cfg.allAtraceApps = val,
            isEnabled: (cfg) => cfg.allAtraceApps,
          } as ToggleAttrs),
          m(AtraceAppsList)),
        m(Probe,
          {
            title: 'Event log (logcat)',
            img: 'rec_logcat.png',
            descr: `Streams the event log into the trace. If no buffer filter is
                      specified, all buffers are selected.`,
            setEnabled: (cfg, val) => cfg.androidLogs = val,
            isEnabled: (cfg) => cfg.androidLogs,
          } as ProbeAttrs,
          m(Dropdown, {
            title: 'Buffers',
            cssClass: '.multicolumn',
            options: LOG_BUFFERS,
            set: (cfg, val) => cfg.androidLogBuffers = val,
            get: (cfg) => cfg.androidLogBuffers,
          } as DropdownAttrs)),
        m(Probe, {
          title: 'Frame timeline',
          img: 'rec_frame_timeline.png',
          descr: `Records expected/actual frame timings from surface_flinger.
                      Requires Android 12 (S) or above.`,
          setEnabled: (cfg, val) => cfg.androidFrameTimeline = val,
          isEnabled: (cfg) => cfg.androidFrameTimeline,
        } as ProbeAttrs));
  }
}
