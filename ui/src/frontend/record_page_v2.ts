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
import {Attributes} from 'mithril';

import {assertExists} from '../base/logging';
import {Actions} from '../common/actions';
import {
  RecordingConfigUtils,
} from '../common/recordingV2/recording_config_utils';
import {
  ChromeTargetInfo,
  RecordingTargetV2,
  TargetInfo,
} from '../common/recordingV2/recording_interfaces_v2';
import {
  RecordingPageController,
  RecordingState,
} from '../common/recordingV2/recording_page_controller';
import {
  EXTENSION_NAME,
  EXTENSION_URL,
} from '../common/recordingV2/recording_utils';
import {
  targetFactoryRegistry,
} from '../common/recordingV2/target_factory_registry';

import {globals} from './globals';
import {fullscreenModalContainer} from './modal';
import {createPage, PageAttrs} from './pages';
import {recordConfigStore} from './record_config';
import {
  Configurations,
  maybeGetActiveCss,
  PERSIST_CONFIG_FLAG,
  RECORDING_SECTIONS,
} from './record_page';
import {CodeSnippet} from './record_widgets';
import {AdvancedSettings} from './recording/advanced_settings';
import {AndroidSettings} from './recording/android_settings';
import {ChromeSettings} from './recording/chrome_settings';
import {CpuSettings} from './recording/cpu_settings';
import {GpuSettings} from './recording/gpu_settings';
import {MemorySettings} from './recording/memory_settings';
import {PowerSettings} from './recording/power_settings';
import {RecordingSectionAttrs} from './recording/recording_sections';
import {RecordingSettings} from './recording/recording_settings';
import {
  FORCE_RESET_MESSAGE,
} from './recording/recording_ui_utils';
import {addNewTarget} from './recording/reset_target_modal';

const START_RECORDING_MESSAGE = 'Start Recording';

const controller = new RecordingPageController();
const recordConfigUtils = new RecordingConfigUtils();
// Whether the target selection modal is displayed.
let shouldDisplayTargetModal: boolean = false;

// Options for displaying a target selection menu.
export interface TargetSelectionOptions {
  // css attributes passed to the mithril components which displays the target
  // selection menu.
  attributes: Attributes;
  // Whether the selection should be preceded by a text label.
  shouldDisplayLabel: boolean;
}

function isChromeTargetInfo(targetInfo: TargetInfo):
    targetInfo is ChromeTargetInfo {
  return ['CHROME', 'CHROME_OS'].includes(targetInfo.targetType);
}

function RecordHeader() {
  const platformSelection = RecordingPlatformSelection();
  const statusLabel = RecordingStatusLabel();
  const buttons = RecordingButton();
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
  if (controller.getState() >= RecordingState.RECORDING) return undefined;

  return m(
      '.target',
      m('.chip',
        {
          onclick: () => {
            shouldDisplayTargetModal = true;
            fullscreenModalContainer.createNew(addNewTargetModal());
            globals.rafScheduler.scheduleFullRedraw();
          },
        },
        m('button', 'Add new recording target'),
        m('i.material-icons', 'add')),
      targetSelection());
}

function addNewTargetModal() {
  return {
    ...addNewTarget(controller),
    onClose: () => shouldDisplayTargetModal = false,
  };
}

export function targetSelection(): m.Vnode|undefined {
  if (!controller.shouldShowTargetSelection()) {
    return undefined;
  }

  const targets: RecordingTargetV2[] = targetFactoryRegistry.listTargets();
  const targetNames = [];
  const targetInfo = controller.getTargetInfo();
  if (!targetInfo) {
    targetNames.push(m('option', 'PLEASE_SELECT_TARGET'));
  }

  let selectedIndex = 0;
  for (let i = 0; i < targets.length; i++) {
    const targetName = targets[i].getInfo().name;
    targetNames.push(m('option', targetName));
    if (targetInfo && targetName === targetInfo.name) {
      selectedIndex = i;
    }
  }

  return m(
      'label',
      'Target platform:',
      m('select',
        {
          selectedIndex,
          onchange: (e: Event) => {
            controller.onTargetSelection((e.target as HTMLSelectElement).value);
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
        ...targetNames),
  );
}

// This will display status messages which are informative, but do not require
// user action, such as: "Recording in progress for X seconds" in the recording
// page header.
function RecordingStatusLabel() {
  const recordingStatus = globals.state.recordingStatus;
  if (!recordingStatus) return undefined;
  return m('label', recordingStatus);
}

function Instructions(cssClass: string) {
  if (controller.getState() < RecordingState.TARGET_SELECTED) {
    return undefined;
  }
  // We will have a valid target at this step because we checked the state.
  const targetInfo = assertExists(controller.getTargetInfo());

  return m(
      `.record-section.instructions${cssClass}`,
      m('header', 'Recording command'),
      (PERSIST_CONFIG_FLAG.get()) ?
          m('button.permalinkconfig',
            {
              onclick: () => {
                globals.dispatch(
                    Actions.createPermalink({isRecordingConfig: true}));
              },
            },
            'Share recording settings') :
          null,
      RecordingSnippet(targetInfo),
      BufferUsageProgressBar(),
      m('.buttons', StopCancelButtons()));
}

function BufferUsageProgressBar() {
  // Show the Buffer Usage bar only after we start recording a trace.
  if (controller.getState() !== RecordingState.RECORDING) {
    return undefined;
  }

  controller.fetchBufferUsage();

  const bufferUsage = controller.getBufferUsagePercentage();
  // Buffer usage is not available yet on Android.
  if (bufferUsage === 0) return undefined;

  return m(
      'label',
      'Buffer usage: ',
      m('progress', {max: 100, value: bufferUsage * 100}));
}

function RecordingNotes() {
  if (controller.getState() !== RecordingState.TARGET_INFO_DISPLAYED) {
    return undefined;
  }
  // We will have a valid target at this step because we checked the state.
  const targetInfo = assertExists(controller.getTargetInfo());

  const linuxUrl = 'https://perfetto.dev/docs/quickstart/linux-tracing';
  const cmdlineUrl =
      'https://perfetto.dev/docs/quickstart/android-tracing#perfetto-cmdline';

  const notes: m.Children = [];

  const msgFeatNotSupported =
      m('span', `Some probes are only supported in Perfetto versions running
      on Android Q+. Therefore, Perfetto will sideload the latest version onto 
      the device.`);

  const msgPerfettoNotSupported = m(
      'span',
      `Perfetto is not supported natively before Android P. Therefore, Perfetto 
       will sideload the latest version onto the device.`);

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

  if (!recordConfigUtils
           .fetchLatestRecordCommand(globals.state.recordConfig, targetInfo)
           .hasDataSources) {
    notes.push(
        m('.note',
          'It looks like you didn\'t add any probes. ' +
              'Please add at least one to get a non-empty trace.'));
  }

  targetFactoryRegistry.listRecordingProblems().map((recordingProblem) => {
    if (recordingProblem.includes(EXTENSION_URL)) {
      // Special case for rendering the link to the Chrome extension.
      const parts = recordingProblem.split(EXTENSION_URL);
      notes.push(
          m('.note',
            parts[0],
            m('a', {href: EXTENSION_URL, target: '_blank'}, EXTENSION_NAME),
            parts[1]));
    }
  });

  switch (targetInfo.targetType) {
    case 'LINUX':
      notes.push(msgLinux);
      break;
    case 'ANDROID': {
      const androidApiLevel = targetInfo.androidApiLevel;
      if (androidApiLevel === 28) {
        notes.push(m('.note', msgFeatNotSupported));
      } else if (androidApiLevel && androidApiLevel <= 27) {
        notes.push(m('.note', msgPerfettoNotSupported));
      }
      break;
    }
    default:
  }

  if (globals.state.recordConfig.mode === 'LONG_TRACE') {
    notes.unshift(msgLongTraces);
  }

  return notes.length > 0 ? m('div', notes) : undefined;
}

function RecordingSnippet(targetInfo: TargetInfo) {
  // We don't need commands to start tracing on chrome
  if (isChromeTargetInfo(targetInfo)) {
    if (controller.getState() > RecordingState.AUTH_P2) {
      // If the UI has started tracing, don't display a message guiding the user
      // to start recording.
      return undefined;
    }
    return m(
        'div',
        m('label', `To trace Chrome from the Perfetto UI you just have to press
         '${START_RECORDING_MESSAGE}'.`));
  }
  return m(CodeSnippet, {text: getRecordCommand(targetInfo)});
}

function getRecordCommand(targetInfo: TargetInfo): string {
  const recordCommand = recordConfigUtils.fetchLatestRecordCommand(
      globals.state.recordConfig, targetInfo);

  const pbBase64 = recordCommand ? recordCommand.configProtoBase64 : '';
  const pbtx = recordCommand ? recordCommand.configProtoText : '';
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

function RecordingButton() {
  if (controller.getState() !== RecordingState.TARGET_INFO_DISPLAYED ||
      !controller.canCreateTracingSession()) {
    return undefined;
  }

  // We know we have a target because we checked the state.
  const targetInfo = assertExists(controller.getTargetInfo());
  const hasDataSources =
      recordConfigUtils
          .fetchLatestRecordCommand(globals.state.recordConfig, targetInfo)
          .hasDataSources;
  if (!hasDataSources) {
    return undefined;
  }

  return m(
      '.button',
      m('button',
        {
          class: 'selected',
          onclick: () => controller.onStartRecordingPressed(),
        },
        START_RECORDING_MESSAGE));
}

function StopCancelButtons() {
  // Show the Stop/Cancel buttons only while we are recording a trace.
  if (!controller.shouldShowStopCancelButtons()) {
    return undefined;
  }

  const stop =
      m(`button.selected`, {onclick: () => controller.onStop()}, 'Stop');

  const cancel = m(`button`, {onclick: () => controller.onCancel()}, 'Cancel');

  return [stop, cancel];
}

function recordMenu(routePage: string) {
  const chromeProbe =
      m('a[href="#!/record/chrome"]',
        m(`li${routePage === 'chrome' ? '.active' : ''}`,
          m('i.material-icons', 'laptop_chromebook'),
          m('.title', 'Chrome'),
          m('.sub', 'Chrome traces')));
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

  // We only display the probes when we have a valid target, so it's not
  // possible for the target to be undefined here.
  const targetType = assertExists(controller.getTargetInfo()).targetType;
  const probes = [];
  if (targetType === 'CHROME_OS' || targetType === 'LINUX') {
    probes.push(cpuProbe, powerProbe, memoryProbe, chromeProbe, advancedProbe);
  } else if (targetType === 'CHROME') {
    probes.push(chromeProbe);
  } else {
    probes.push(
        cpuProbe,
        gpuProbe,
        powerProbe,
        memoryProbe,
        androidProbe,
        chromeProbe,
        advancedProbe);
  }

  return m(
      '.record-menu',
      {
        class: controller.getState() > RecordingState.TARGET_INFO_DISPLAYED ?
            'disabled' :
            '',
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

function getRecordContainer(subpage?: string): m.Vnode<any, any> {
  const components: m.Children[] = [RecordHeader()];
  if (controller.getState() === RecordingState.NO_TARGET) {
    components.push(m('.full-centered', 'Please connect a valid target.'));
    return m('.record-container', components);
  } else if (controller.getState() <= RecordingState.ASK_TO_FORCE_P1) {
    components.push(
        m('.full-centered',
          'Can not access the device without resetting the ' +
              `connection. Please refresh the page, then click ` +
              `'${FORCE_RESET_MESSAGE}.'`));
    return m('.record-container', components);
  } else if (controller.getState() === RecordingState.AUTH_P1) {
    components.push(
        m('.full-centered', 'Please allow USB debugging on the device.'));
    return m('.record-container', components);
  } else if (
      controller.getState() === RecordingState.WAITING_FOR_TRACE_DISPLAY) {
    components.push(
        m('.full-centered', 'Waiting for the trace to be collected.'));
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
    ['chrome', ChromeSettings],
    ['advanced', AdvancedSettings],
  ]);
  for (const [section, component] of settingsSections.entries()) {
    pages.push(m(component, {
      dataSources: controller.getTargetInfo()?.dataSources || [],
      cssClass: maybeGetActiveCss(routePage, section),
    } as RecordingSectionAttrs));
  }

  components.push(m('.record-container-content', pages));
  return m('.record-container', components);
}

export const RecordPageV2 = createPage({

  oninit(): void {
    controller.initFactories();
  },

  view({attrs}: m.Vnode<PageAttrs>): void |
      m.Children {
        if (shouldDisplayTargetModal) {
          fullscreenModalContainer.updateVdom(addNewTargetModal());
        }

        return m(
            '.record-page',
            controller.getState() > RecordingState.TARGET_INFO_DISPLAYED ?
                m('.hider') :
                [],
            getRecordContainer(attrs.subpage));
      },
});
