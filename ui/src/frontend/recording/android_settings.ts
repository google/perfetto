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

import {DataSourceDescriptor} from '../../protos';
import {globals} from '../globals';
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

const LOG_BUFFERS = new Map<string, string>();
LOG_BUFFERS.set('LID_CRASH', 'Crash');
LOG_BUFFERS.set('LID_DEFAULT', 'Main');
LOG_BUFFERS.set('LID_EVENTS', 'Binary events');
LOG_BUFFERS.set('LID_KERNEL', 'Kernel');
LOG_BUFFERS.set('LID_RADIO', 'Radio');
LOG_BUFFERS.set('LID_SECURITY', 'Security');
LOG_BUFFERS.set('LID_STATS', 'Stats');
LOG_BUFFERS.set('LID_SYSTEM', 'System');

const DEFAULT_ATRACE_CATEGORIES = new Map<string, string>();
DEFAULT_ATRACE_CATEGORIES.set('adb', 'ADB');
DEFAULT_ATRACE_CATEGORIES.set('aidl', 'AIDL calls');
DEFAULT_ATRACE_CATEGORIES.set('am', 'Activity Manager');
DEFAULT_ATRACE_CATEGORIES.set('audio', 'Audio');
DEFAULT_ATRACE_CATEGORIES.set('binder_driver', 'Binder Kernel driver');
DEFAULT_ATRACE_CATEGORIES.set('binder_lock', 'Binder global lock trace');
DEFAULT_ATRACE_CATEGORIES.set('bionic', 'Bionic C library');
DEFAULT_ATRACE_CATEGORIES.set('camera', 'Camera');
DEFAULT_ATRACE_CATEGORIES.set('dalvik', 'ART & Dalvik');
DEFAULT_ATRACE_CATEGORIES.set('database', 'Database');
DEFAULT_ATRACE_CATEGORIES.set('gfx', 'Graphics');
DEFAULT_ATRACE_CATEGORIES.set('hal', 'Hardware Modules');
DEFAULT_ATRACE_CATEGORIES.set('input', 'Input');
DEFAULT_ATRACE_CATEGORIES.set('network', 'Network');
DEFAULT_ATRACE_CATEGORIES.set('nnapi', 'Neural Network API');
DEFAULT_ATRACE_CATEGORIES.set('pm', 'Package Manager');
DEFAULT_ATRACE_CATEGORIES.set('power', 'Power Management');
DEFAULT_ATRACE_CATEGORIES.set('res', 'Resource Loading');
DEFAULT_ATRACE_CATEGORIES.set('rro', 'Resource Overlay');
DEFAULT_ATRACE_CATEGORIES.set('rs', 'RenderScript');
DEFAULT_ATRACE_CATEGORIES.set('sm', 'Sync Manager');
DEFAULT_ATRACE_CATEGORIES.set('ss', 'System Server');
DEFAULT_ATRACE_CATEGORIES.set('vibrator', 'Vibrator');
DEFAULT_ATRACE_CATEGORIES.set('video', 'Video');
DEFAULT_ATRACE_CATEGORIES.set('view', 'View System');
DEFAULT_ATRACE_CATEGORIES.set('webview', 'WebView');
DEFAULT_ATRACE_CATEGORIES.set('wm', 'Window Manager');

function isDataSourceDescriptor(descriptor: unknown):
    descriptor is DataSourceDescriptor {
  if (descriptor instanceof Object) {
    return (descriptor as DataSourceDescriptor).name !== undefined;
  }
  return false;
}

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
      cssClass: '.record-apps-list',
      set: (cfg, val) => cfg.atraceApps = val,
      get: (cfg) => cfg.atraceApps,
    } as TextareaAttrs);
  }
}

export class AndroidSettings implements
    m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    let atraceCategories = DEFAULT_ATRACE_CATEGORIES;
    for (const dataSource of attrs.dataSources) {
      if (dataSource.name !== 'linux.ftrace' ||
          !isDataSourceDescriptor(dataSource.descriptor)) {
        continue;
      }
      const atraces = dataSource.descriptor.ftraceDescriptor?.atraceCategories;
      if (!atraces || atraces.length === 0) {
        break;
      }

      atraceCategories = new Map<string, string>();
      for (const atrace of atraces) {
        if (atrace.name) {
          atraceCategories.set(atrace.name, atrace.description || '');
        }
      }
    }

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
            options: atraceCategories,
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
        } as ProbeAttrs),
        m(Probe, {
          title: 'Game intervention list',
          img: '',
          descr: `List game modes and interventions.
                    Requires Android 13 (T) or above.`,
          setEnabled: (cfg, val) => cfg.androidGameInterventionList = val,
          isEnabled: (cfg) => cfg.androidGameInterventionList,
        } as ProbeAttrs),
        m(Probe,
          {
            title: 'Network Tracing',
            img: '',
            descr: `Records detailed information on network packets.
                      Requires Android 14 (U) or above.`,
            setEnabled: (cfg, val) => cfg.androidNetworkTracing = val,
            isEnabled: (cfg) => cfg.androidNetworkTracing,
          } as ProbeAttrs,
          m(Slider, {
            title: 'Poll interval',
            cssClass: '.thin',
            values: [100, 250, 500, 1000, 2500],
            unit: 'ms',
            set: (cfg, val) => cfg.androidNetworkTracingPollMs = val,
            get: (cfg) => cfg.androidNetworkTracingPollMs,
          } as SliderAttrs)));
  }
}
