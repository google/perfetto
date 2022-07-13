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
import {TraceConfig} from '../common/protos';
import {
  genTraceConfig,
  RecordingConfigUtils,
} from '../common/recordingV2/recording_config_utils';
import {
  RecordingError,
  showRecordingModal,
} from '../common/recordingV2/recording_error_handling';
import {
  OnTargetChangeCallback,
  RecordingTargetV2,
  TargetInfo,
  TracingSession,
  TracingSessionListener,
} from '../common/recordingV2/recording_interfaces_v2';
import {
  ANDROID_WEBSOCKET_TARGET_FACTORY,
  AndroidWebsocketTargetFactory,
} from
    '../common/recordingV2/target_factories/android_websocket_target_factory';
import {
  ANDROID_WEBUSB_TARGET_FACTORY,
} from '../common/recordingV2/target_factories/android_webusb_target_factory';
import {
  targetFactoryRegistry,
} from '../common/recordingV2/target_factory_registry';
import {
  RECORDING_IN_PROGRESS,
} from '../common/recordingV2/traced_tracing_session';
import {hasActiveProbes} from '../common/state';
import {currentDateHourAndMinute} from '../common/time';

import {globals} from './globals';
import {createPage, PageAttrs} from './pages';
import {publishBufferUsage} from './publish';
import {autosaveConfigStore, recordConfigStore} from './record_config';
import {
  Configurations,
  maybeGetActiveCss,
  PERSIST_CONFIG_FLAG,
  RECORDING_SECTIONS,
} from './record_page';
import {CodeSnippet} from './record_widgets';
import {AdvancedSettings} from './recording/advanced_settings';
import {AndroidSettings} from './recording/android_settings';
import {CpuSettings} from './recording/cpu_settings';
import {GpuSettings} from './recording/gpu_settings';
import {MemorySettings} from './recording/memory_settings';
import {PowerSettings} from './recording/power_settings';
import {couldNotClaimInterface} from './recording/recording_modal';
import {RecordingSectionAttrs} from './recording/recording_sections';
import {RecordingSettings} from './recording/recording_settings';

// Wraps all calls to a recording target and handles the errors that can be
// thrown during these calls.
async function connectToRecordingTarget(
    target: RecordingTargetV2,
    tracingSessionListener: TracingSessionListener,
    executeConnection: () => Promise<void>) {
  const createSession = async () => {
    try {
      await executeConnection();
    } catch (e) {
      tracingSessionListener.onError(e.message);
    }
  };

  if (await target.canConnectWithoutContention()) {
    await createSession();
  } else {
    couldNotClaimInterface(createSession);
  }
}

// Wraps a tracing session promise while the promise is being resolved (e.g.
// while we are awaiting for ADB auth).
class TracingSessionWrapper {
  private tracingSession?: TracingSession = undefined;
  private isCancelled = false;

  constructor(private traceConfig: TraceConfig, target: RecordingTargetV2) {
    connectToRecordingTarget(target, tracingSessionListener, async () => {
      const session = await target.createTracingSession(tracingSessionListener);
      this.onSessionPromiseResolved(session);
    });
  }

  cancel() {
    if (!this.tracingSession) {
      this.isCancelled = true;
      return;
    }
    this.tracingSession.cancel();
  }

  stop() {
    if (!this.tracingSession) {
      this.isCancelled = true;
      return;
    }
    this.tracingSession.stop();
  }

  getTraceBufferUsage(): Promise<number>|undefined {
    if (!this.tracingSession) {
      return undefined;
    }
    return this.tracingSession.getTraceBufferUsage();
  }

  private onSessionPromiseResolved(session: TracingSession) {
    // We cancel the received trace if it is marked as cancelled. For instance:
    // - The user clicked 'Start', then 'Stop' without authorizing, then 'Start'
    // and then authorized.
    if (this.isCancelled) {
      session.cancel();
      return;
    }

    this.tracingSession = session;
    this.tracingSession.start(this.traceConfig);
    globals.rafScheduler.scheduleFullRedraw();
  }
}

const adbWebsocketUrl = 'ws://127.0.0.1:8037/adb';
const recordConfigUtils = new RecordingConfigUtils();
let recordingTargetV2: RecordingTargetV2|undefined = undefined;
let tracingSessionWrapper: TracingSessionWrapper|undefined = undefined;

const tracingSessionListener: TracingSessionListener = {
  onTraceData: (trace: Uint8Array) => {
    globals.dispatch(Actions.openTraceFromBuffer({
      title: 'Recorded trace',
      buffer: trace.buffer,
      fileName: `trace_${currentDateHourAndMinute()}${TRACE_SUFFIX}`,
    }));
    clearRecordingState();
  },
  onStatus: (message: string) => {
    // For the 'Recording in progress for 7000ms we don't show a modal.'
    if (message.startsWith(RECORDING_IN_PROGRESS)) {
      globals.dispatch(Actions.setRecordingStatus({status: message}));
    } else {
      // For messages such as 'Please allow USB debugging on your device, which
      // require a user action, we show a modal.
      showRecordingModal(message);
    }
  },
  onDisconnect: (errorMessage?: string) => {
    if (errorMessage) {
      showRecordingModal(errorMessage);
    }
    clearRecordingState();
  },
  onError: (message: string) => {
    showRecordingModal(message);
    clearRecordingState();
  },
};

function RecordHeader() {
  const platformSelection = RecordingPlatformSelection();
  const statusLabel = RecordingStatusLabel();
  const buttons = RecordingButtons();
  const notes = RecordingNotes();
  if (!platformSelection && !statusLabel && !buttons && !notes) {
    // The header should not be displayed when it has no content.
    return undefined;
  }
  return m(
      '.record-header',
      m('.top-part',
        m('.target-and-status', platformSelection, statusLabel),
        buttons),
      notes);
}


function RecordingPlatformSelection() {
  // Don't show the platform selector while we are recording a trace.
  if (tracingSessionWrapper) return undefined;

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
              onTargetSelection((e.target as HTMLSelectElement).value);
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

// This will display status messages which are informative, but do not require
// user action, such as: "Recording in progress for X seconds" in the recording
// page header.
function RecordingStatusLabel() {
  const recordingStatus = globals.state.recordingStatus;
  if (!recordingStatus) return undefined;
  return m('label', recordingStatus);
}

async function addAndroidDevice(): Promise<void> {
  try {
    const target =
        await targetFactoryRegistry.get(ANDROID_WEBUSB_TARGET_FACTORY)
            .connectNewTarget();
    await assignRecordingTarget(target);
  } catch (e) {
    if (e instanceof RecordingError) {
      showRecordingModal(e.message);
    } else {
      throw e;
    }
  }
}

function onTargetSelection(targetName: string): void {
  const allTargets = targetFactoryRegistry.listTargets();
  assignRecordingTarget(
      allTargets.find((t) => t.getInfo().name === targetName) || allTargets[0]);
}

function Instructions(cssClass: string) {
  return m(
      `.record-section.instructions${cssClass}`,
      m('header', 'Recording command'),
      (PERSIST_CONFIG_FLAG.get() && !tracingSessionWrapper) ?
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
      m('.buttons', StopCancelButtons()));
}

function BufferUsageProgressBar() {
  const bufferUsagePromise = tracingSessionWrapper?.getTraceBufferUsage();
  if (!bufferUsagePromise) {
    return undefined;
  }

  bufferUsagePromise.then((percentage) => {
    publishBufferUsage({percentage});
  });

  const bufferUsage = globals.bufferUsage ? globals.bufferUsage : 0.0;
  // Buffer usage is not available yet on Android.
  if (bufferUsage === 0) return undefined;

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
        const androidApiLevel = targetInfo.androidApiLevel;
        if (androidApiLevel === 28) {
          notes.push(m('.note', msgFeatNotSupported, msgSideload));
        } else if (androidApiLevel && androidApiLevel <= 27) {
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

  return notes.length > 0 ? m('div', notes) : undefined;
}

function RecordingSnippet() {
  const targetInfo = assertExists(recordingTargetV2).getInfo();
  // We don't need commands to start tracing on chrome
  if (targetInfo.targetType === 'CHROME') {
    if (!globals.state.extensionInstalled) return undefined;
    return m(
        'div',
        m('label', `To trace Chrome from the Perfetto UI you just have to press
         'Start Recording'.`));
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

function RecordingButtons() {
  // We don't show the 'Start Recording' button if:
  // A. There is no connected target.
  // B. We have already started tracing.
  // C. There is a connected Android target but we don't have user authorisation
  // to record a trace.
  if (!recordingTargetV2) {
    return undefined;
  }

  // We don't allow the user to press 'Start Recording' multiple times
  // because this will create multiple connecting tracing sessions, which
  // will block one another.
  if (tracingSessionWrapper) {
    return undefined;
  }

  const targetInfo = recordingTargetV2.getInfo();
  // The absence of androidApiLevel shows that we have not connected to the
  // device, therefore we can not start recording.
  // TODO(octaviant): encapsulation should be stricter here, look into making
  // this a method
  if (targetInfo.targetType === 'ANDROID' && !targetInfo.androidApiLevel) {
    return undefined;
  }

  const start =
      m(`button`,
        {
          class: tracingSessionWrapper ? '' : 'selected',
          onclick: onStartRecordingPressed,
        },
        'Start Recording');

  const buttons: m.Children = [];
  const targetType = targetInfo.targetType;
  if (targetType === 'ANDROID' &&
      globals.state.recordConfig.mode !== 'LONG_TRACE') {
    buttons.push(start);
  } else if (targetType === 'CHROME' && globals.state.extensionInstalled) {
    buttons.push(start);
  }
  return m('.button', buttons);
}

function StopCancelButtons() {
  // Show the Stop/Cancel buttons only while we are recording a trace.
  if (!tracingSessionWrapper) return undefined;

  const stop =
      m(`button.selected`,
        {
          onclick: () => {
            assertExists(tracingSessionWrapper).stop();
            clearRecordingState();
          },
        },
        'Stop');

  const cancel =
      m(`button`,
        {
          onclick: () => {
            assertExists(tracingSessionWrapper).cancel();
            clearRecordingState();
          },
        },
        'Cancel');

  return [stop, cancel];
}

function onStartRecordingPressed(): void {
  location.href = '#!/record/instructions';
  autosaveConfigStore.save(globals.state.recordConfig);

  const target = assertExists(recordingTargetV2);
  const targetInfo = target.getInfo();
  if (targetInfo.targetType === 'ANDROID' ||
      targetInfo.targetType === 'CHROME') {
    globals.logging.logEvent(
        'Record Trace',
        `Record trace (${targetInfo.targetType}${targetInfo.targetType})`);
    const traceConfig = genTraceConfig(globals.state.recordConfig, targetInfo);
    tracingSessionWrapper = new TracingSessionWrapper(traceConfig, target);
  }
  globals.rafScheduler.scheduleFullRedraw();
}

function clearRecordingState() {
  publishBufferUsage({percentage: 0});
  globals.dispatch(Actions.setRecordingStatus({status: undefined}));
  tracingSessionWrapper = undefined;
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
        class: tracingSessionWrapper ? 'disabled' : '',
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

const onTargetChange: OnTargetChangeCallback = () => {
  const allTargets = targetFactoryRegistry.listTargets();
  if (recordingTargetV2 && allTargets.includes(recordingTargetV2)) {
    globals.rafScheduler.scheduleFullRedraw();
    return;
  }
  assignRecordingTarget(allTargets[0]);
};

async function assignRecordingTarget(selectedTarget?: RecordingTargetV2) {
  // If the selected target is the same as the previous one, we don't need to
  // do anything.
  if (selectedTarget === recordingTargetV2) {
    return;
  }

  // We assign the new target and redraw the page.
  recordingTargetV2 = selectedTarget;
  globals.rafScheduler.scheduleFullRedraw();

  if (!recordingTargetV2) {
    return;
  }

  await connectToRecordingTarget(
      recordingTargetV2, tracingSessionListener, async () => {
        if (!recordingTargetV2) {
          return;
        }
        await recordingTargetV2.fetchTargetInfo(tracingSessionListener);
      });
}

function getRecordContainer(subpage?: string): m.Vnode<any, any> {
  const components: m.Children[] = [RecordHeader()];
  if (!recordingTargetV2) {
    components.push(m('.full-centered', 'Please connect a valid target.'));
    return m('.record-container', components);
  }

  const targetInfo = recordingTargetV2.getInfo();
  // The absence of androidApiLevel shows that we have not connected to the
  // device because we do not have user authorization.
  if (targetInfo.targetType === 'ANDROID' && !targetInfo.androidApiLevel) {
    components.push(
        m('.full-centered', 'Please allow USB debugging on the device.'));
    return m('.record-container', components);
  }

  const pages: m.Children = [];
  // we need to remove the `/` character from the route
  let routePage = subpage ? subpage.substr(1) : '';
  if (!RECORDING_SECTIONS.includes(routePage)) {
    routePage = 'buffers';
  }
  pages.push(recordMenu(routePage));

  pages.push(m(RecordingSettings, {
    dataSources: [],
    cssClass: maybeGetActiveCss(routePage, 'buffers'),
  } as RecordingSectionAttrs));
  pages.push(Instructions(maybeGetActiveCss(routePage, 'instructions')));
  pages.push(Configurations(maybeGetActiveCss(routePage, 'config')));

  const settingsSections = new Map([
    ['cpu', CpuSettings],
    ['gpu', GpuSettings],
    ['power', PowerSettings],
    ['memory', MemorySettings],
    ['android', AndroidSettings],
    ['advanced', AdvancedSettings],
    // TODO(octaviant): Add Chrome settings.
  ]);
  for (const [section, component] of settingsSections.entries()) {
    pages.push(m(component, {
      dataSources: [],
      cssClass: maybeGetActiveCss(routePage, section),
    } as RecordingSectionAttrs));
  }

  components.push(m('.record-container-content', pages));
  return m('.record-container', components);
}

export const RecordPageV2 = createPage({

  oninit(): void {
    for (const targetFactory of targetFactoryRegistry.listTargetFactories()) {
      if (targetFactory && !targetFactory.onTargetChange) {
        targetFactory.onTargetChange = onTargetChange;
      }
    }

    if (targetFactoryRegistry.has(ANDROID_WEBSOCKET_TARGET_FACTORY)) {
      const websocketTargetFactory =
          targetFactoryRegistry.get(ANDROID_WEBSOCKET_TARGET_FACTORY) as
          AndroidWebsocketTargetFactory;
      websocketTargetFactory.tryEstablishWebsocket(adbWebsocketUrl);
    }
  },

  view({attrs}: m.Vnode<PageAttrs>): void |
      m.Children {
        if (!recordingTargetV2) {
          assignRecordingTarget(targetFactoryRegistry.listTargets()[0]);
        }

        return m(
            '.record-page',
            tracingSessionWrapper ? m('.hider') : [],
            getRecordContainer(attrs.subpage));
      },
});
