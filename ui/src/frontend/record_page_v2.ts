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

import {assertExists} from '../base/logging';
import {Actions} from '../common/actions';
import {TRACE_SUFFIX} from '../common/constants';
import {
  genTraceConfig,
  RecordingConfigUtils,
} from '../common/recordingV2/recording_config_utils';
import {
  showRecordingModal,
  wrapRecordingError,
} from '../common/recordingV2/recording_error_handling';
import {
  OnTargetChangedCallback,
  RecordingTargetV2,
  TargetInfo,
  TracingSession,
} from '../common/recordingV2/recording_interfaces_v2';
import {
  ANDROID_WEBUSB_TARGET_FACTORY,
} from '../common/recordingV2/target_factories/android_webusb_target_factory';
import {
  targetFactoryRegistry,
} from '../common/recordingV2/target_factory_registry';
import {hasActiveProbes} from '../common/state';

import {globals} from './globals';
import {createPage, PageAttrs} from './pages';
import {publishBufferUsage} from './publish';
import {autosaveConfigStore, recordConfigStore} from './record_config';
import {
  AdvancedSettings,
  AndroidSettings,
  Configurations,
  CpuSettings,
  ErrorLabel,
  GpuSettings,
  MemorySettings,
  PERSIST_CONFIG_FLAG,
  PowerSettings,
  RecordingStatusLabel,
  RecSettings,
} from './record_page';
import {CodeSnippet} from './record_widgets';

const recordConfigUtils = new RecordingConfigUtils();
let recordingTargetV2: RecordingTargetV2|undefined = undefined;
let tracingSession: Promise<TracingSession>|undefined = undefined;


function RecordHeader() {
  return m(
      '.record-header',
      m('.top-part',
        m('.target-and-status',
          RecordingPlatformSelection(),
          RecordingStatusLabel(),
          ErrorLabel()),
        recordingButtons()),
      RecordingNotes());
}

function RecordingPlatformSelection() {
  if (tracingSession) return [];

  const components = [];
  components.push(
      m('.chip',
        {onclick: addAndroidDevice},
        m('button', 'Add ADB Device'),
        m('i.material-icons', 'add')));

  if (recordingTargetV2) {
    const targets = [];
    let selectedIndex = 0;
    for (const [i, target] of targetFactoryRegistry.listTargets().entries()) {
      targets.push(m('option', target.getInfo().name));
      if (recordingTargetV2 &&
          target.getInfo().name === recordingTargetV2.getInfo().name) {
        selectedIndex = i;
      }
    }
    components.push(m(
        'label',
        'Target platform:',
        m('select',
          {
            selectedIndex,
            onchange: (e: Event) => {
              onTargetChange((e.target as HTMLSelectElement).value);
            },
            onupdate: (select) => {
              // Work around mithril bug
              // (https://github.com/MithrilJS/mithril.js/issues/2107): We may
              // update the select's options while also changing the
              // selectedIndex at the same time. The update of selectedIndex
              // may be applied before the new options are added to the select
              // element. Because the new selectedIndex may be outside of the
              // select's options at that time, we have to reselect the
              // correct index here after any new children were added.
              (select.dom as HTMLSelectElement).selectedIndex = selectedIndex;
            },
          },
          ...targets),
        ));
  }

  return m('.target', components);
}

async function addAndroidDevice(): Promise<void> {
  const target = await wrapRecordingError(
      targetFactoryRegistry.get(ANDROID_WEBUSB_TARGET_FACTORY)
          .connectNewTarget(),
      (message) => showRecordingModal(message));
  if (target) {
    recordingTargetV2 = target;
    globals.rafScheduler.scheduleFullRedraw();
  }
}

function onTargetChange(targetName: string): void {
  const allTargets = targetFactoryRegistry.listTargets();
  recordingTargetV2 =
      allTargets.find((t) => t.getInfo().name === targetName) || allTargets[0];
  globals.rafScheduler.scheduleFullRedraw();
}

function Instructions(cssClass: string) {
  return m(
      `.record-section.instructions${cssClass}`,
      m('header', 'Recording command'),
      (PERSIST_CONFIG_FLAG.get() && !tracingSession) ?
          m('button.permalinkconfig',
            {
              onclick: () => {
                globals.dispatch(
                    Actions.createPermalink({isRecordingConfig: true}));
              },
            },
            'Share recording settings') :
          null,
      RecordingSnippet(),
      BufferUsageProgressBar(),
      m('.buttons', StopCancelButtons()),
      recordingLog());
}

function BufferUsageProgressBar() {
  if (!tracingSession) return [];

  tracingSession.then((session) => session.getTraceBufferUsage())
      .then((percentage) => {
        publishBufferUsage({percentage});
      });

  const bufferUsage = globals.bufferUsage ? globals.bufferUsage : 0.0;
  // Buffer usage is not available yet on Android.
  if (bufferUsage === 0) return [];

  return m(
      'label',
      'Buffer usage: ',
      m('progress', {max: 100, value: bufferUsage * 100}));
}

function RecordingNotes() {
  const sideloadUrl =
      'https://perfetto.dev/docs/contributing/build-instructions#get-the-code';
  const linuxUrl = 'https://perfetto.dev/docs/quickstart/linux-tracing';
  const cmdlineUrl =
      'https://perfetto.dev/docs/quickstart/android-tracing#perfetto-cmdline';
  const extensionURL = `https://chrome.google.com/webstore/detail/
      perfetto-ui/lfmkphfpdbjijhpomgecfikhfohaoine`;

  const notes: m.Children = [];

  const msgFeatNotSupported =
      m('span', `Some probes are only supported in Perfetto versions running
      on Android Q+. `);

  const msgPerfettoNotSupported =
      m('span', `Perfetto is not supported natively before Android P. `);

  const msgSideload =
      m('span',
        `If you have a rooted device you can `,
        m('a',
          {href: sideloadUrl, target: '_blank'},
          `sideload the latest version of
         Perfetto.`));

  const msgChrome =
      m('.note',
        `To trace Chrome from the Perfetto UI, you need to install our `,
        m('a', {href: extensionURL, target: '_blank'}, 'Chrome extension'),
        ' and then reload this page.');

  const msgLinux =
      m('.note',
        `Use this `,
        m('a', {href: linuxUrl, target: '_blank'}, `quickstart guide`),
        ` to get started with tracing on Linux.`);

  const msgLongTraces = m(
      '.note',
      `Recording in long trace mode through the UI is not supported. Please copy
    the command and `,
      m('a',
        {href: cmdlineUrl, target: '_blank'},
        `collect the trace using ADB.`));

  const msgZeroProbes =
      m('.note',
        'It looks like you didn\'t add any probes. ' +
            'Please add at least one to get a non-empty trace.');

  if (!hasActiveProbes(globals.state.recordConfig)) {
    notes.push(msgZeroProbes);
  }

  targetFactoryRegistry.listRecordingProblems().map((recordingProblem) => {
    notes.push(m('.note', recordingProblem));
  });

  if (recordingTargetV2) {
    const targetInfo = recordingTargetV2.getInfo();

    switch (targetInfo.targetType) {
      case 'CHROME':
      case 'CHROME_OS':
        if (!globals.state.extensionInstalled) notes.push(msgChrome);
        break;
      case 'LINUX':
        notes.push(msgLinux);
        break;
      case 'ANDROID': {
        if (targetInfo.androidApiLevel == 28) {
          notes.push(m('.note', msgFeatNotSupported, msgSideload));
        } else if (
            targetInfo.androidApiLevel && targetInfo.androidApiLevel <= 27) {
          notes.push(m('.note', msgPerfettoNotSupported, msgSideload));
        }
        break;
      }
      default:
    }
  }

  if (globals.state.recordConfig.mode === 'LONG_TRACE') {
    notes.unshift(msgLongTraces);
  }

  return notes.length > 0 ? m('div', notes) : [];
}

function RecordingSnippet() {
  const targetInfo = assertExists(recordingTargetV2).getInfo();
  // We don't need commands to start tracing on chrome
  if (targetInfo.targetType === 'CHROME') {
    return globals.state.extensionInstalled ?
        m('div',
          m('label',
            `To trace Chrome from the Perfetto UI you just have to press
         'Start Recording'.`)) :
        [];
  }
  return m(CodeSnippet, {text: getRecordCommand(targetInfo)});
}

function getRecordCommand(targetInfo: TargetInfo): string {
  const data = recordConfigUtils.fetchLatestRecordCommand(
      globals.state.recordConfig, assertExists(recordingTargetV2));

  const pbBase64 = data ? data.configProtoBase64 : '';
  const pbtx = data ? data.configProtoText : '';
  let cmd = '';
  if (targetInfo.targetType === 'ANDROID' &&
      targetInfo.androidApiLevel === 28) {
    cmd += `echo '${pbBase64}' | \n`;
    cmd += 'base64 --decode | \n';
    cmd += 'adb shell "perfetto -c - -o /data/misc/perfetto-traces/trace"\n';
  } else {
    cmd += targetInfo.targetType === 'ANDROID' ? 'adb shell perfetto \\\n' :
                                                 'perfetto \\\n';
    cmd += '  -c - --txt \\\n';
    cmd += '  -o /data/misc/perfetto-traces/trace \\\n';
    cmd += '<<EOF\n\n';
    cmd += pbtx;
    cmd += '\nEOF\n';
  }
  return cmd;
}

function recordingButtons() {
  if (!recordingTargetV2) {
    return [];
  }

  const start =
      m(`button`,
        {
          class: tracingSession ? '' : 'selected',
          onclick: onStartRecordingPressed,
        },
        'Start Recording');

  const buttons: m.Children = [];

  const targetType = recordingTargetV2.getInfo().targetType;
  if (targetType === 'ANDROID' && !tracingSession &&
      globals.state.recordConfig.mode !== 'LONG_TRACE') {
    buttons.push(start);
  } else if (targetType === 'CHROME' && globals.state.extensionInstalled) {
    buttons.push(start);
  }
  return m('.button', buttons);
}

function StopCancelButtons() {
  if (!tracingSession) return [];

  const stop =
      m(`button.selected`,
        {
          onclick: async () => {
            assertExists(tracingSession).then(async (session) => {
              // If this tracing session is not the one currently in focus,
              // then we interpret 'Stop' as 'Cancel'. Otherwise, this
              // trace will be processed and rendered even though the user
              // wants to see another trace.
              const ongoingTracingSession = await tracingSession;
              if (ongoingTracingSession && ongoingTracingSession !== session) {
                session.cancel();
              }
              session.stop();
            });
            clearRecordingState();
          },
        },
        'Stop');

  const cancel =
      m(`button`,
        {
          onclick: async () => {
            assertExists(tracingSession).then((session) => session.cancel());
            clearRecordingState();
          },
        },
        'Cancel');

  return [stop, cancel];
}

async function onStartRecordingPressed(): Promise<void> {
  location.href = '#!/record/instructions';
  globals.rafScheduler.scheduleFullRedraw();
  autosaveConfigStore.save(globals.state.recordConfig);

  if (!recordingTargetV2) {
    return;
  }

  const targetInfo = recordingTargetV2.getInfo();
  if (targetInfo.targetType === 'ANDROID' ||
      targetInfo.targetType === 'CHROME') {
    globals.logging.logEvent(
        'Record Trace',
        `Record trace (${targetInfo.targetType}${targetInfo.targetType})`);

    const traceConfig =
        genTraceConfig(globals.state.recordConfig, recordingTargetV2.getInfo());

    const onTraceData = (trace: Uint8Array) => {
      clearRecordingState();
      globals.dispatch(Actions.openTraceFromBuffer({
        title: 'Recorded trace',
        buffer: trace.buffer,
        fileName: `recorded_trace${TRACE_SUFFIX}`,
      }));
    };

    const onStatus = (message: string) => {
      globals.dispatch(Actions.setRecordingStatus({status: message}));
    };

    const onDisconnect = (errorMessage?: string) => {
      clearRecordingState();
      if (errorMessage) {
        showRecordingModal(errorMessage);
      }
    };

    const onError = (message: string) => {
      clearRecordingState();
      showRecordingModal(message);
    };

    const tracingSessionListener =
        {onTraceData, onStatus, onDisconnect, onError};
    tracingSession =
        recordingTargetV2.createTracingSession(tracingSessionListener);
    tracingSession.then((tracingSession) => {
      if (tracingSession === undefined) {
        // if the tracing session was stopped/cancelled, then we don't
        // start the tracing session
        return;
      }
      tracingSession.start(traceConfig);
    });
    wrapRecordingError(tracingSession, onError);
  }
}

function recordingLog() {
  const logs = globals.recordingLog;
  if (logs === undefined) return [];
  return m('.code-snippet.no-top-bar', m('code', logs));
}

function clearRecordingState() {
  publishBufferUsage({percentage: 0});
  globals.dispatch(Actions.setRecordingStatus({status: undefined}));
  tracingSession = undefined;
}

function recordMenu(routePage: string) {
  const cpuProbe =
      m('a[href="#!/record/cpu"]',
        m(`li${routePage === 'cpu' ? '.active' : ''}`,
          m('i.material-icons', 'subtitles'),
          m('.title', 'CPU'),
          m('.sub', 'CPU usage, scheduling, wakeups')));
  const gpuProbe =
      m('a[href="#!/record/gpu"]',
        m(`li${routePage === 'gpu' ? '.active' : ''}`,
          m('i.material-icons', 'aspect_ratio'),
          m('.title', 'GPU'),
          m('.sub', 'GPU frequency, memory')));
  const powerProbe =
      m('a[href="#!/record/power"]',
        m(`li${routePage === 'power' ? '.active' : ''}`,
          m('i.material-icons', 'battery_charging_full'),
          m('.title', 'Power'),
          m('.sub', 'Battery and other energy counters')));
  const memoryProbe =
      m('a[href="#!/record/memory"]',
        m(`li${routePage === 'memory' ? '.active' : ''}`,
          m('i.material-icons', 'memory'),
          m('.title', 'Memory'),
          m('.sub', 'Physical mem, VM, LMK')));
  const androidProbe =
      m('a[href="#!/record/android"]',
        m(`li${routePage === 'android' ? '.active' : ''}`,
          m('i.material-icons', 'android'),
          m('.title', 'Android apps & svcs'),
          m('.sub', 'atrace and logcat')));
  const advancedProbe =
      m('a[href="#!/record/advanced"]',
        m(`li${routePage === 'advanced' ? '.active' : ''}`,
          m('i.material-icons', 'settings'),
          m('.title', 'Advanced settings'),
          m('.sub', 'Complicated stuff for wizards')));

  const targetType = assertExists(recordingTargetV2).getInfo().targetType;
  const probes = [];
  if (targetType === 'CHROME_OS' || targetType === 'LINUX') {
    probes.push(cpuProbe, powerProbe, memoryProbe, advancedProbe);
  } else {
    probes.push(
        cpuProbe,
        gpuProbe,
        powerProbe,
        memoryProbe,
        androidProbe,
        advancedProbe);
  }

  return m(
      '.record-menu',
      {
        class: tracingSession ? 'disabled' : '',
        onclick: () => globals.rafScheduler.scheduleFullRedraw(),
      },
      m('header', 'Trace config'),
      m('ul',
        m('a[href="#!/record/buffers"]',
          m(`li${routePage === 'buffers' ? '.active' : ''}`,
            m('i.material-icons', 'tune'),
            m('.title', 'Recording settings'),
            m('.sub', 'Buffer mode, size and duration'))),
        m('a[href="#!/record/instructions"]',
          m(`li${routePage === 'instructions' ? '.active' : ''}`,
            m('i.material-icons.rec', 'fiber_manual_record'),
            m('.title', 'Recording command'),
            m('.sub', 'Manually record trace'))),
        PERSIST_CONFIG_FLAG.get() ?
            m('a[href="#!/record/config"]',
              {
                onclick: () => {
                  recordConfigStore.reloadFromLocalStorage();
                },
              },
              m(`li${routePage === 'config' ? '.active' : ''}`,
                m('i.material-icons', 'save'),
                m('.title', 'Saved configs'),
                m('.sub', 'Manage local configs'))) :
            null),
      m('header', 'Probes'),
      m('ul', probes));
}

const onDevicesChanged: OnTargetChangedCallback = () => {
  recordingTargetV2 = targetFactoryRegistry.listTargets()[0];
  // redraw, to add/remove connected/disconnected target
  globals.rafScheduler.scheduleFullRedraw();
};

export const RecordPageV2 = createPage({
  view({attrs}: m.Vnode<PageAttrs>): void |
      m.Children {
        if (!recordingTargetV2) {
          recordingTargetV2 = targetFactoryRegistry.listTargets()[0];
        }

        const androidWebusbTarget =
            targetFactoryRegistry.get(ANDROID_WEBUSB_TARGET_FACTORY);
        if (!androidWebusbTarget.onDevicesChanged) {
          androidWebusbTarget.onDevicesChanged = onDevicesChanged;
        }

        const components: m.Children[] = [RecordHeader()];
        if (recordingTargetV2) {
          const SECTIONS:
              {[property: string]: (cssClass: string) => m.Child} = {
                buffers: RecSettings,
                instructions: Instructions,
                config: Configurations,
                cpu: CpuSettings,
                gpu: GpuSettings,
                power: PowerSettings,
                memory: MemorySettings,
                android: AndroidSettings,
                advanced: AdvancedSettings,
              };

          const pages: m.Children = [];
          // we need to remove the `/` character from the route
          let routePage = attrs.subpage ? attrs.subpage.substr(1) : '';
          if (!Object.keys(SECTIONS).includes(routePage)) {
            routePage = 'buffers';
          }
          for (const key of Object.keys(SECTIONS)) {
            const cssClass = routePage === key ? '.active' : '';
            pages.push(SECTIONS[key](cssClass));
          }
          components.push(recordMenu(routePage));
          components.push(...pages);
        }

        return m(
            '.record-page',
            tracingSession ? m('.hider') : [],
            m('.record-container', components));
      },
});
