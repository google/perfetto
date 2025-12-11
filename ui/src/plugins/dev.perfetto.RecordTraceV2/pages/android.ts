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
import {RecordSubpage, RecordProbe} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';
import {TypedMultiselect} from './widgets/multiselect';
import {POLL_INTERVAL_SLIDER, Slider} from './widgets/slider';
import {Textarea} from './widgets/textarea';
import {Toggle} from './widgets/toggle';

export function androidRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'android',
    title: 'Android apps & svcs',
    subtitle: 'Android-specific data sources',
    icon: 'android',
    probes: [
      atrace(),
      logcat(),
      frameTimeline(),
      gameInterventions(),
      netTracing(),
      statsdAtoms(),
    ],
  };
}

function atrace(): RecordProbe {
  const options = new Map<string, string>();
  const defaultSelected: string[] = [];
  for (const [id, {title, isDefault}] of ATRACE_CATEGORIES) {
    const key = `${id}: ${title}`;
    options.set(key, id);
    if (isDefault) {
      defaultSelected.push(key);
    }
  }
  const settings = {
    categories: new TypedMultiselect<string>({
      options,
      defaultSelected,
    }),
    apps: new Textarea({
      title: 'Process / package names to trace',
      placeholder: 'e.g. system_server\ncom.android.settings',
    }),
    allApps: new Toggle({
      title: 'Record events from all Android apps and services',
      cssClass: '.thin',
      onChange(allAppsEnabled: boolean) {
        settings.apps.attrs.disabled = allAppsEnabled;
      },
    }),
  };
  return {
    id: 'atrace',
    title: 'Atrace userspace annotations',
    image: 'rec_atrace.png',
    description:
      'Enables C++ / Java codebase annotations (ATRACE_BEGIN() / os.Trace())',
    supportedPlatforms: ['ANDROID'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addAtraceCategories(...settings.categories.selectedValues());
      if (settings.allApps.enabled) {
        tc.addAtraceApps('*');
      } else {
        for (const line of splitLinesNonEmpty(settings.apps.text)) {
          tc.addAtraceApps(line);
        }
      }
      if (
        settings.categories.selectedKeys().length > 0 ||
        settings.allApps.enabled
      ) {
        tc.addFtraceEvents('ftrace/print');
      }
    },
  };
}

function logcat(): RecordProbe {
  const settings = {
    buffers: new TypedMultiselect<protos.AndroidLogId>({
      options: new Map(
        Object.entries({
          'Crash': protos.AndroidLogId.LID_CRASH,
          'Main': protos.AndroidLogId.LID_DEFAULT,
          'Binary events': protos.AndroidLogId.LID_EVENTS,
          'Kernel': protos.AndroidLogId.LID_KERNEL,
          'Radio': protos.AndroidLogId.LID_RADIO,
          'Security': protos.AndroidLogId.LID_SECURITY,
          'Stats': protos.AndroidLogId.LID_STATS,
          'System': protos.AndroidLogId.LID_SYSTEM,
        }),
      ),
    }),
  };
  return {
    id: 'logcat',
    title: 'Event log (logcat)',
    image: 'rec_logcat.png',
    description:
      'Streams the event log into the trace. If no buffer filter is ' +
      'specified, all buffers are selected.',
    supportedPlatforms: ['ANDROID'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const logIds = settings.buffers.selectedValues();
      tc.addDataSource('android.log').androidLogConfig = {
        logIds: logIds.length > 0 ? logIds : undefined,
      };
    },
  };
}

function frameTimeline(): RecordProbe {
  return {
    id: 'android_frame_timeline',
    title: 'Frame timeline',
    description:
      'Records expected/actual frame timings from surface_flinger.' +
      'Requires Android 12 (S) or above.',
    supportedPlatforms: ['ANDROID'],
    docsLink: 'https://perfetto.dev/docs/data-sources/frametimeline',
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addDataSource('android.surfaceflinger.frametimeline');
    },
  };
}

function gameInterventions(): RecordProbe {
  return {
    id: 'android_game_interventions',
    title: 'Game intervention list',
    description:
      'List game modes and interventions. Requires Android 13 (T) or above.',
    supportedPlatforms: ['ANDROID'],
    docsLink:
      'https://perfetto.dev/docs/data-sources/android-game-intervention-list',
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addDataSource('android.game_interventions');
    },
  };
}

function netTracing(): RecordProbe {
  const settings = {pollMs: new Slider(POLL_INTERVAL_SLIDER)};
  return {
    id: 'network_tracing',
    title: 'Network Tracing',
    description:
      'Records detailed information on network packets. ' +
      'Requires Android 14 (U) or above',
    supportedPlatforms: ['ANDROID'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addDataSource('android.network_packets').networkPacketTraceConfig = {
        pollMs: settings.pollMs.value,
      };
      // Allows mapping packet uids to package names.
      tc.addDataSource('android.packages_list');
    },
  };
}

function statsdAtoms(): RecordProbe {
  const settings = {
    pushAtoms: new TypedMultiselect<protos.AtomId>({
      title: 'Push atoms',
      options: new Map(
        Object.entries(protos.AtomId)
          .filter(([_, v]) => typeof v === 'number' && v > 2 && v < 9999)
          .map(([k, v]) => [k, v as protos.AtomId]),
      ),
    }),
    rawPushIds: new Textarea({
      placeholder:
        'Add raw pushed atoms IDs, one per line, e.g.:\n' + '818\n' + '819',
    }),
    pullAtoms: new TypedMultiselect<protos.AtomId>({
      title: 'Pull atoms',
      options: new Map(
        Object.entries(protos.AtomId)
          .filter(([_, v]) => typeof v === 'number' && v > 10000 && v < 99999)
          .map(([k, v]) => [k, v as protos.AtomId]),
      ),
    }),
    rawPullIds: new Textarea({
      placeholder:
        'Add raw pulled atom IDs, one per line, e.g.:\n10063\n10064\n',
    }),
    pullInterval: new Slider({...POLL_INTERVAL_SLIDER, default: 5000}),
    pullPkg: new Textarea({
      placeholder:
        'Add pulled atom packages, one per line, e.g.:\n' +
        'com.android.providers.telephony',
    }),
  };
  return {
    id: 'statsd',
    title: 'Statsd atoms',
    description: 'Record instances of statsd atoms to the Statsd Atoms track.',
    supportedPlatforms: ['ANDROID'],
    docsLink:
      'https://cs.android.com/android/platform/superproject/main/+/main:frameworks/proto_logging/stats/atoms.proto',
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const pkg = splitLinesNonEmpty(settings.pullPkg.text);
      const pullIds = settings.pullAtoms.selectedValues();
      const rawPullIds = splitLinesNonEmpty(settings.rawPullIds.text).map((l) =>
        parseInt(l.trim()),
      );
      const hasPull = pullIds.length > 0 || rawPullIds.length > 0;
      tc.addDataSource('android.statsd').statsdTracingConfig = {
        pushAtomId: settings.pushAtoms.selectedValues(),
        rawPushAtomId: splitLinesNonEmpty(settings.rawPushIds.text).map((l) =>
          parseInt(l.trim()),
        ),
        pullConfig: hasPull
          ? [
              {
                pullAtomId: pullIds,
                rawPullAtomId: rawPullIds,
                pullFrequencyMs: settings.pullInterval.value,
                packages: pkg.length > 0 ? pkg : undefined,
              },
            ]
          : undefined,
      };
    },
  };
}

// Defaults are from
// https://android.googlesource.com/platform/packages/apps/Traceur/+/refs/heads/main/src_common/com/android/traceur/PresetTraceConfigs.java
const ATRACE_CATEGORIES = new Map<string, {title: string; isDefault: boolean}>([
  ['adb', {title: 'ADB', isDefault: false}],
  ['aidl', {title: 'AIDL calls', isDefault: true}],
  ['am', {title: 'Activity Manager', isDefault: true}],
  ['audio', {title: 'Audio', isDefault: false}],
  ['binder_driver', {title: 'Binder Kernel driver', isDefault: true}],
  ['binder_lock', {title: 'Binder global lock trace', isDefault: false}],
  ['bionic', {title: 'Bionic C Library', isDefault: false}],
  ['camera', {title: 'Camera', isDefault: true}],
  ['dalvik', {title: 'Dalvik VM', isDefault: true}],
  ['database', {title: 'Database', isDefault: false}],
  ['disk', {title: 'Disk I/O', isDefault: true}],
  ['freq', {title: 'CPU Frequency', isDefault: true}],
  ['gfx', {title: 'Graphics', isDefault: true}],
  ['hal', {title: 'Hardware Modules', isDefault: true}],
  ['idle', {title: 'CPU Idle', isDefault: true}],
  ['input', {title: 'Input', isDefault: true}],
  ['memory', {title: 'Memory', isDefault: true}],
  ['memreclaim', {title: 'Kernel Memory Reclaim', isDefault: true}],
  ['network', {title: 'Network', isDefault: true}],
  ['nnapi', {title: 'NNAPI', isDefault: false}],
  ['pm', {title: 'Package Manager', isDefault: false}],
  ['power', {title: 'Power Management', isDefault: true}],
  ['res', {title: 'Resource Loading', isDefault: true}],
  ['rro', {title: 'Runtime Resource Overlay', isDefault: false}],
  ['rs', {title: 'RenderScript', isDefault: false}],
  ['sched', {title: 'CPU Scheduling', isDefault: true}],
  ['sm', {title: 'Sync Manager', isDefault: false}],
  ['ss', {title: 'System Server', isDefault: true}],
  ['sync', {title: 'Synchronization', isDefault: true}],
  ['thermal', {title: 'Thermal event', isDefault: true}],
  ['vibrator', {title: 'Vibrator', isDefault: false}],
  ['video', {title: 'Video', isDefault: false}],
  ['view', {title: 'View System', isDefault: true}],
  ['webview', {title: 'WebView', isDefault: true}],
  ['wm', {title: 'Window Manager', isDefault: true}],
  ['workq', {title: 'Kernel Workqueues', isDefault: true}],
]);
