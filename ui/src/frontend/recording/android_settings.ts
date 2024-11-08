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
import {AtomId, DataSourceDescriptor} from '../../protos';
import {Dropdown, Probe, Slider, Textarea, Toggle} from '../record_widgets';
import {RecordingSectionAttrs} from './recording_sections';
import {RecordConfig} from '../../controller/record_config_types';

const PUSH_ATOM_IDS = new Map<string, string>();
const PULL_ATOM_IDS = new Map<string, string>();
for (const key in AtomId) {
  if (!Object.hasOwn(AtomId, key)) continue;
  const value = Number(AtomId[key]);
  if (!isNaN(value)) {
    if (value > 2 && value < 9999) {
      PUSH_ATOM_IDS.set(String(value), key);
    } else if (value >= 10000 && value < 99999) {
      PULL_ATOM_IDS.set(String(value), key);
    }
  }
}

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

function isDataSourceDescriptor(
  descriptor: unknown,
): descriptor is DataSourceDescriptor {
  if (descriptor instanceof Object) {
    return (descriptor as DataSourceDescriptor).name !== undefined;
  }
  return false;
}

interface AtraceAppsListAttrs {
  recCfg: RecordConfig;
}

class AtraceAppsList implements m.ClassComponent<AtraceAppsListAttrs> {
  view({attrs}: m.CVnode<AtraceAppsListAttrs>) {
    if (attrs.recCfg.allAtraceApps) {
      return m('div');
    }

    return m(Textarea, {
      placeholder:
        'Apps to profile, one per line, e.g.:\n' +
        'com.android.phone\n' +
        'lmkd\n' +
        'com.android.nfc',
      cssClass: '.record-apps-list',
      set: (cfg, val) => (cfg.atraceApps = val),
      get: (cfg) => cfg.atraceApps,
      recCfg: attrs.recCfg,
    });
  }
}

export class AndroidSettings
  implements m.ClassComponent<RecordingSectionAttrs>
{
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    const recCfg = attrs.recState.recordConfig;
    let atraceCategories = DEFAULT_ATRACE_CATEGORIES;
    for (const dataSource of attrs.dataSources) {
      if (
        dataSource.name !== 'linux.ftrace' ||
        !isDataSourceDescriptor(dataSource.descriptor)
      ) {
        continue;
      }
      const atraces = dataSource.descriptor.ftraceDescriptor?.atraceCategories;
      if (!atraces || atraces.length === 0) {
        break;
      }

      atraceCategories = new Map<string, string>();
      for (const atrace of atraces) {
        if (atrace.name) {
          atraceCategories.set(atrace.name, atrace.description ?? '');
        }
      }
    }

    return m(
      `.record-section${attrs.cssClass}`,
      m(
        Probe,
        {
          title: 'Atrace userspace annotations',
          img: 'rec_atrace.png',
          descr: `Enables C++ / Java codebase annotations (ATRACE_BEGIN() /
                      os.Trace())`,
          setEnabled: (cfg, val) => (cfg.atrace = val),
          isEnabled: (cfg) => cfg.atrace,
          recCfg,
        },
        m(Dropdown, {
          title: 'Categories',
          cssClass: '.multicolumn.atrace-categories',
          options: atraceCategories,
          set: (cfg, val) => (cfg.atraceCats = val),
          get: (cfg) => cfg.atraceCats,
          recCfg,
        }),
        m(Toggle, {
          title: 'Record events from all Android apps and services',
          descr: '',
          setEnabled: (cfg, val) => (cfg.allAtraceApps = val),
          isEnabled: (cfg) => cfg.allAtraceApps,
          recCfg,
        }),
        m(AtraceAppsList, {recCfg}),
      ),
      m(
        Probe,
        {
          title: 'Event log (logcat)',
          img: 'rec_logcat.png',
          descr: `Streams the event log into the trace. If no buffer filter is
                      specified, all buffers are selected.`,
          setEnabled: (cfg, val) => (cfg.androidLogs = val),
          isEnabled: (cfg) => cfg.androidLogs,
          recCfg,
        },
        m(Dropdown, {
          title: 'Buffers',
          cssClass: '.multicolumn',
          options: LOG_BUFFERS,
          set: (cfg, val) => (cfg.androidLogBuffers = val),
          get: (cfg) => cfg.androidLogBuffers,
          recCfg,
        }),
      ),
      m(Probe, {
        title: 'Frame timeline',
        img: 'rec_frame_timeline.png',
        descr: `Records expected/actual frame timings from surface_flinger.
                      Requires Android 12 (S) or above.`,
        setEnabled: (cfg, val) => (cfg.androidFrameTimeline = val),
        isEnabled: (cfg) => cfg.androidFrameTimeline,
        recCfg,
      }),
      m(Probe, {
        title: 'Game intervention list',
        img: '',
        descr: `List game modes and interventions.
                    Requires Android 13 (T) or above.`,
        setEnabled: (cfg, val) => (cfg.androidGameInterventionList = val),
        isEnabled: (cfg) => cfg.androidGameInterventionList,
        recCfg,
      }),
      m(
        Probe,
        {
          title: 'Network Tracing',
          img: '',
          descr: `Records detailed information on network packets.
                      Requires Android 14 (U) or above.`,
          setEnabled: (cfg, val) => (cfg.androidNetworkTracing = val),
          isEnabled: (cfg) => cfg.androidNetworkTracing,
          recCfg,
        },
        m(Slider, {
          title: 'Poll interval',
          cssClass: '.thin',
          values: [100, 250, 500, 1000, 2500],
          unit: 'ms',
          set: (cfg, val) => (cfg.androidNetworkTracingPollMs = val),
          get: (cfg) => cfg.androidNetworkTracingPollMs,
          recCfg,
        }),
      ),
      m(
        Probe,
        {
          title: 'Statsd Atoms',
          img: '',
          descr:
            "Record instances of statsd atoms to the 'Statsd Atoms' track.",
          setEnabled: (cfg, val) => (cfg.androidStatsd = val),
          isEnabled: (cfg) => cfg.androidStatsd,
          recCfg,
        },
        m(Dropdown, {
          title: 'Pushed Atoms',
          cssClass: '.singlecolumn',
          options: PUSH_ATOM_IDS,
          set: (cfg, val) => (cfg.androidStatsdPushedAtoms = val),
          get: (cfg) => cfg.androidStatsdPushedAtoms,
          recCfg,
        }),
        m(Textarea, {
          placeholder:
            'Add raw pushed atoms IDs, one per line, e.g.:\n' + '818\n' + '819',
          set: (cfg, val) => (cfg.androidStatsdRawPushedAtoms = val),
          get: (cfg) => cfg.androidStatsdRawPushedAtoms,
          recCfg,
        }),
        m(Dropdown, {
          title: 'Pulled Atoms',
          cssClass: '.singlecolumn',
          options: PULL_ATOM_IDS,
          set: (cfg, val) => (cfg.androidStatsdPulledAtoms = val),
          get: (cfg) => cfg.androidStatsdPulledAtoms,
          recCfg,
        }),
        m(Textarea, {
          placeholder:
            'Add raw pulled atom IDs, one per line, e.g.:\n' +
            '10063\n' +
            '10064\n',
          set: (cfg, val) => (cfg.androidStatsdRawPulledAtoms = val),
          get: (cfg) => cfg.androidStatsdRawPulledAtoms,
          recCfg,
        }),
        m(Slider, {
          title: 'Pulled atom pull frequency (ms)',
          cssClass: '.thin',
          values: [500, 1000, 5000, 30000, 60000],
          unit: 'ms',
          set: (cfg, val) => (cfg.androidStatsdPulledAtomPullFrequencyMs = val),
          get: (cfg) => cfg.androidStatsdPulledAtomPullFrequencyMs,
          recCfg,
        }),
        m(Textarea, {
          placeholder:
            'Add pulled atom packages, one per line, e.g.:\n' +
            'com.android.providers.telephony',
          set: (cfg, val) => (cfg.androidStatsdPulledAtomPackages = val),
          get: (cfg) => cfg.androidStatsdPulledAtomPackages,
          recCfg,
        }),
      ),
    );
  }
}
