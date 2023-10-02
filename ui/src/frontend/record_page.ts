// Copyright (C) 2018 The Android Open Source Project
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

import {Actions} from '../common/actions';
import {featureFlags} from '../common/feature_flags';
import {
  AdbRecordingTarget,
  getDefaultRecordingTargets,
  hasActiveProbes,
  isAdbTarget,
  isAndroidP,
  isAndroidTarget,
  isChromeTarget,
  isCrOSTarget,
  isLinuxTarget,
  LoadedConfig,
  MAX_TIME,
  RecordingTarget,
} from '../common/state';
import {AdbOverWebUsb} from '../controller/adb';
import {
  createEmptyRecordConfig,
  RecordConfig,
} from '../controller/record_config_types';
import {raf} from '../core/raf_scheduler';

import {globals} from './globals';
import {createPage, PageAttrs} from './pages';
import {
  autosaveConfigStore,
  recordConfigStore,
  recordTargetStore,
} from './record_config';
import {
  CodeSnippet,
} from './record_widgets';
import {AdvancedSettings} from './recording/advanced_settings';
import {AndroidSettings} from './recording/android_settings';
import {ChromeSettings} from './recording/chrome_settings';
import {CpuSettings} from './recording/cpu_settings';
import {GpuSettings} from './recording/gpu_settings';
import {LinuxPerfSettings} from './recording/linux_perf_settings';
import {MemorySettings} from './recording/memory_settings';
import {PowerSettings} from './recording/power_settings';
import {RecordingSectionAttrs} from './recording/recording_sections';
import {RecordingSettings} from './recording/recording_settings';

export const PERSIST_CONFIG_FLAG = featureFlags.register({
  id: 'persistConfigsUI',
  name: 'Config persistence UI',
  description: 'Show experimental config persistence UI on the record page.',
  defaultValue: true,
});

export const RECORDING_SECTIONS = [
  'buffers',
  'instructions',
  'config',
  'cpu',
  'gpu',
  'power',
  'memory',
  'android',
  'chrome',
  'tracePerf',
  'advanced',
];

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
  if (globals.state.recordingInProgress) return [];

  const availableAndroidDevices = globals.state.availableAdbDevices;
  const recordingTarget = globals.state.recordingTarget;

  const targets = [];
  for (const {os, name} of getDefaultRecordingTargets()) {
    targets.push(m('option', {value: os}, name));
  }
  for (const d of availableAndroidDevices) {
    targets.push(m('option', {value: d.serial}, d.name));
  }

  const selectedIndex = isAdbTarget(recordingTarget) ?
      targets.findIndex((node) => node.attrs.value === recordingTarget.serial) :
      targets.findIndex((node) => node.attrs.value === recordingTarget.os);

  return m(
      '.target',
      m(
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
          ),
      m('.chip',
        {onclick: addAndroidDevice},
        m('button', 'Add ADB Device'),
        m('i.material-icons', 'add')));
}

// |target| can be the TargetOs or the android serial.
function onTargetChange(target: string) {
  const recordingTarget: RecordingTarget =
      globals.state.availableAdbDevices.find((d) => d.serial === target) ||
      getDefaultRecordingTargets().find((t) => t.os === target) ||
      getDefaultRecordingTargets()[0];

  if (isChromeTarget(recordingTarget)) {
    globals.dispatch(Actions.setFetchChromeCategories({fetch: true}));
  }

  globals.dispatch(Actions.setRecordingTarget({target: recordingTarget}));
  recordTargetStore.save(target);
  raf.scheduleFullRedraw();
}

function Instructions(cssClass: string) {
  return m(
      `.record-section.instructions${cssClass}`,
      m('header', 'Recording command'),
      PERSIST_CONFIG_FLAG.get() ?
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

export function loadedConfigEqual(
    cfg1: LoadedConfig, cfg2: LoadedConfig): boolean {
  return cfg1.type === 'NAMED' && cfg2.type === 'NAMED' ?
      cfg1.name === cfg2.name :
      cfg1.type === cfg2.type;
}

export function loadConfigButton(
    config: RecordConfig, configType: LoadedConfig): m.Vnode {
  return m(
      'button',
      {
        class: 'config-button',
        title: 'Apply configuration settings',
        disabled: loadedConfigEqual(configType, globals.state.lastLoadedConfig),
        onclick: () => {
          globals.dispatch(Actions.setRecordConfig({config, configType}));
          raf.scheduleFullRedraw();
        },
      },
      m('i.material-icons', 'file_upload'));
}

export function displayRecordConfigs() {
  const configs = [];
  if (autosaveConfigStore.hasSavedConfig) {
    configs.push(m('.config', [
      m('span.title-config', m('strong', 'Latest started recording')),
      loadConfigButton(autosaveConfigStore.get(), {type: 'AUTOMATIC'}),
    ]));
  }
  for (const validated of recordConfigStore.recordConfigs) {
    const item = validated.result;
    configs.push(m('.config', [
      m('span.title-config', item.title),
      loadConfigButton(item.config, {type: 'NAMED', name: item.title}),
      m('button',
        {
          class: 'config-button',
          title: 'Overwrite configuration with current settings',
          onclick: () => {
            if (confirm(`Overwrite config "${
                    item.title}" with current settings?`)) {
              recordConfigStore.overwrite(globals.state.recordConfig, item.key);
              globals.dispatch(Actions.setRecordConfig({
                config: item.config,
                configType: {type: 'NAMED', name: item.title},
              }));
              raf.scheduleFullRedraw();
            }
          },
        },
        m('i.material-icons', 'save')),
      m('button',
        {
          class: 'config-button',
          title: 'Remove configuration',
          onclick: () => {
            recordConfigStore.delete(item.key);
            raf.scheduleFullRedraw();
          },
        },
        m('i.material-icons', 'delete')),
    ]));

    const errorItems = [];
    for (const extraKey of validated.extraKeys) {
      errorItems.push(m('li', `${extraKey} is unrecognised`));
    }
    for (const invalidKey of validated.invalidKeys) {
      errorItems.push(m('li', `${invalidKey} contained an invalid value`));
    }

    if (errorItems.length > 0) {
      configs.push(
          m('.parsing-errors',
            'One or more errors have been found while loading configuration "' +
                item.title + '". Loading is possible, but make sure to check ' +
                'the settings afterwards.',
            m('ul', errorItems)));
    }
  }
  return configs;
}

export const ConfigTitleState = {
  title: '',
  getTitle: () => {
    return ConfigTitleState.title;
  },
  setTitle: (value: string) => {
    ConfigTitleState.title = value;
  },
  clearTitle: () => {
    ConfigTitleState.title = '';
  },
};

export function Configurations(cssClass: string) {
  const canSave = recordConfigStore.canSave(ConfigTitleState.getTitle());
  return m(
      `.record-section${cssClass}`,
      m('header', 'Save and load configurations'),
      m('.input-config',
        [
          m('input', {
            value: ConfigTitleState.title,
            placeholder: 'Title for config',
            oninput() {
              ConfigTitleState.setTitle(this.value);
              raf.scheduleFullRedraw();
            },
          }),
          m('button',
            {
              class: 'config-button',
              disabled: !canSave,
              title: canSave ? 'Save current config' :
                               'Duplicate name, saving disabled',
              onclick: () => {
                recordConfigStore.save(
                    globals.state.recordConfig, ConfigTitleState.getTitle());
                raf.scheduleFullRedraw();
                ConfigTitleState.clearTitle();
              },
            },
            m('i.material-icons', 'save')),
          m('button',
            {
              class: 'config-button',
              title: 'Clear current configuration',
              onclick: () => {
                if (confirm(
                        'Current configuration will be cleared. ' +
                        'Are you sure?')) {
                  globals.dispatch(Actions.setRecordConfig({
                    config: createEmptyRecordConfig(),
                    configType: {type: 'NONE'},
                  }));
                  raf.scheduleFullRedraw();
                }
              },
            },
            m('i.material-icons', 'delete_forever')),
        ]),
      displayRecordConfigs());
}

function BufferUsageProgressBar() {
  if (!globals.state.recordingInProgress) return [];

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
  const extensionURL =
      `https://chrome.google.com/webstore/detail/perfetto-ui/lfmkphfpdbjijhpomgecfikhfohaoine`;

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

  const msgRecordingNotSupported =
      m('.note',
        `Recording Perfetto traces from the UI is not supported natively
     before Android Q. If you are using a P device, please select 'Android P'
     as the 'Target Platform' and `,
        m('a',
          {href: cmdlineUrl, target: '_blank'},
          `collect the trace using ADB.`));

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

  if (isAdbTarget(globals.state.recordingTarget)) {
    notes.push(msgRecordingNotSupported);
  }
  switch (globals.state.recordingTarget.os) {
    case 'Q':
      break;
    case 'P':
      notes.push(m('.note', msgFeatNotSupported, msgSideload));
      break;
    case 'O':
      notes.push(m('.note', msgPerfettoNotSupported, msgSideload));
      break;
    case 'L':
      notes.push(msgLinux);
      break;
    case 'C':
      if (!globals.state.extensionInstalled) notes.push(msgChrome);
      break;
    case 'CrOS':
      if (!globals.state.extensionInstalled) notes.push(msgChrome);
      break;
    default:
  }
  if (globals.state.recordConfig.mode === 'LONG_TRACE') {
    notes.unshift(msgLongTraces);
  }

  return notes.length > 0 ? m('div', notes) : [];
}

function RecordingSnippet() {
  const target = globals.state.recordingTarget;

  // We don't need commands to start tracing on chrome
  if (isChromeTarget(target)) {
    return globals.state.extensionInstalled &&
            !globals.state.recordingInProgress ?
        m('div',
          m('label',
            `To trace Chrome from the Perfetto UI you just have to press
         'Start Recording'.`)) :
        [];
  }
  return m(CodeSnippet, {text: getRecordCommand(target)});
}

function getRecordCommand(target: RecordingTarget) {
  const data = globals.trackDataStore.get('config') as
          {commandline: string, pbtxt: string, pbBase64: string} |
      null;

  const cfg = globals.state.recordConfig;
  let time = cfg.durationMs / 1000;

  if (time > MAX_TIME) {
    time = MAX_TIME;
  }

  const pbBase64 = data ? data.pbBase64 : '';
  const pbtx = data ? data.pbtxt : '';
  let cmd = '';
  if (isAndroidP(target)) {
    cmd += `echo '${pbBase64}' | \n`;
    cmd += 'base64 --decode | \n';
    cmd += 'adb shell "perfetto -c - -o /data/misc/perfetto-traces/trace"\n';
  } else {
    cmd +=
        isAndroidTarget(target) ? 'adb shell perfetto \\\n' : 'perfetto \\\n';
    cmd += '  -c - --txt \\\n';
    cmd += '  -o /data/misc/perfetto-traces/trace \\\n';
    cmd += '<<EOF\n\n';
    cmd += pbtx;
    cmd += '\nEOF\n';
  }
  return cmd;
}

function recordingButtons() {
  const state = globals.state;
  const target = state.recordingTarget;
  const recInProgress = state.recordingInProgress;

  const start =
      m(`button`,
        {
          class: recInProgress ? '' : 'selected',
          onclick: onStartRecordingPressed,
        },
        'Start Recording');

  const buttons: m.Children = [];

  if (isAndroidTarget(target)) {
    if (!recInProgress && isAdbTarget(target) &&
        globals.state.recordConfig.mode !== 'LONG_TRACE') {
      buttons.push(start);
    }
  } else if (isChromeTarget(target) && state.extensionInstalled) {
    buttons.push(start);
  }
  return m('.button', buttons);
}

function StopCancelButtons() {
  if (!globals.state.recordingInProgress) return [];

  const stop =
      m(`button.selected`,
        {onclick: () => globals.dispatch(Actions.stopRecording({}))},
        'Stop');

  const cancel =
      m(`button`,
        {onclick: () => globals.dispatch(Actions.cancelRecording({}))},
        'Cancel');

  return [stop, cancel];
}

function onStartRecordingPressed() {
  location.href = '#!/record/instructions';
  raf.scheduleFullRedraw();
  autosaveConfigStore.save(globals.state.recordConfig);

  const target = globals.state.recordingTarget;
  if (isAndroidTarget(target) || isChromeTarget(target)) {
    globals.logging.logEvent('Record Trace', `Record trace (${target.os})`);
    globals.dispatch(Actions.startRecording({}));
  }
}

function RecordingStatusLabel() {
  const recordingStatus = globals.state.recordingStatus;
  if (!recordingStatus) return [];
  return m('label', recordingStatus);
}

export function ErrorLabel() {
  const lastRecordingError = globals.state.lastRecordingError;
  if (!lastRecordingError) return [];
  return m('label.error-label', `Error:  ${lastRecordingError}`);
}

function recordingLog() {
  const logs = globals.recordingLog;
  if (logs === undefined) return [];
  return m('.code-snippet.no-top-bar', m('code', logs));
}

// The connection must be done in the frontend. After it, the serial ID will
// be inserted in the state, and the worker will be able to connect to the
// correct device.
async function addAndroidDevice() {
  let device: USBDevice;
  try {
    device = await new AdbOverWebUsb().findDevice();
  } catch (e) {
    const err = `No device found: ${e.name}: ${e.message}`;
    console.error(err, e);
    alert(err);
    return;
  }

  if (!device.serialNumber) {
    console.error('serial number undefined');
    return;
  }

  // After the user has selected a device with the chrome UI, it will be
  // available when listing all the available device from WebUSB. Therefore,
  // we update the list of available devices.
  await updateAvailableAdbDevices(device.serialNumber);
}

// We really should be getting the API version from the adb target, but
// currently its too complicated to do that (== most likely, we need to finish
// recordingV2 migration). For now, add an escape hatch to use Android S as a
// default, given that the main features we want are gated by API level 31 and S
// is old enough to be the default most of the time.
const USE_ANDROID_S_AS_DEFAULT_FLAG = featureFlags.register({
  id: 'recordingPageUseSAsDefault',
  name: 'Use Android S as a default recording target',
  description: 'Use Android S as a default recording target instead of Q',
  defaultValue: false,
});

export async function updateAvailableAdbDevices(
    preferredDeviceSerial?: string) {
  const devices = await new AdbOverWebUsb().getPairedDevices();

  let recordingTarget: AdbRecordingTarget|undefined = undefined;

  const availableAdbDevices: AdbRecordingTarget[] = [];
  devices.forEach((d) => {
    if (d.productName && d.serialNumber) {
      // TODO(nicomazz): At this stage, we can't know the OS version, so we
      // assume it is 'Q'. This can create problems with devices with an old
      // version of perfetto. The os detection should be done after the adb
      // connection, from adb_record_controller
      availableAdbDevices.push({
        name: d.productName,
        serial: d.serialNumber,
        os: USE_ANDROID_S_AS_DEFAULT_FLAG.get() ? 'S' : 'Q',
      });
      if (preferredDeviceSerial && preferredDeviceSerial === d.serialNumber) {
        recordingTarget = availableAdbDevices[availableAdbDevices.length - 1];
      }
    }
  });

  globals.dispatch(
      Actions.setAvailableAdbDevices({devices: availableAdbDevices}));
  selectAndroidDeviceIfAvailable(availableAdbDevices, recordingTarget);
  raf.scheduleFullRedraw();
  return availableAdbDevices;
}

function selectAndroidDeviceIfAvailable(
    availableAdbDevices: AdbRecordingTarget[],
    recordingTarget?: RecordingTarget) {
  if (!recordingTarget) {
    recordingTarget = globals.state.recordingTarget;
  }
  const deviceConnected = isAdbTarget(recordingTarget);
  const connectedDeviceDisconnected = deviceConnected &&
      availableAdbDevices.find(
          (e) => e.serial ===
              (recordingTarget as AdbRecordingTarget).serial) === undefined;

  if (availableAdbDevices.length) {
    // If there's an Android device available and the current selection isn't
    // one, select the Android device by default. If the current device isn't
    // available anymore, but another Android device is, select the other
    // Android device instead.
    if (!deviceConnected || connectedDeviceDisconnected) {
      recordingTarget = availableAdbDevices[0];
    }

    globals.dispatch(Actions.setRecordingTarget({target: recordingTarget}));
    return;
  }

  // If the currently selected device was disconnected, reset the recording
  // target to the default one.
  if (connectedDeviceDisconnected) {
    globals.dispatch(
        Actions.setRecordingTarget({target: getDefaultRecordingTargets()[0]}));
  }
}

function recordMenu(routePage: string) {
  const target = globals.state.recordingTarget;
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
  const tracePerfProbe =
      m('a[href="#!/record/tracePerf"]',
        m(`li${routePage === 'tracePerf' ? '.active' : ''}`,
          m('i.material-icons', 'full_stacked_bar_chart'),
          m('.title', 'Stack Samples'),
          m('.sub', 'Lightweight stack polling')));
  const recInProgress = globals.state.recordingInProgress;

  const probes = [];
  if (isCrOSTarget(target) || isLinuxTarget(target)) {
    probes.push(cpuProbe, powerProbe, memoryProbe, chromeProbe, advancedProbe);
  } else if (isChromeTarget(target)) {
    probes.push(chromeProbe);
  } else {
    probes.push(
        cpuProbe,
        gpuProbe,
        powerProbe,
        memoryProbe,
        androidProbe,
        chromeProbe,
        tracePerfProbe,
        advancedProbe);
  }

  return m(
      '.record-menu',
      {
        class: recInProgress ? 'disabled' : '',
        onclick: () => raf.scheduleFullRedraw(),
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
            m('i.material-icons-filled.rec', 'fiber_manual_record'),
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

export function maybeGetActiveCss(routePage: string, section: string): string {
  return routePage === section ? '.active' : '';
}

export const RecordPage = createPage({
  view({attrs}: m.Vnode<PageAttrs>) {
    const pages: m.Children = [];
    // we need to remove the `/` character from the route
    let routePage = attrs.subpage ? attrs.subpage.substr(1) : '';
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
      ['tracePerf', LinuxPerfSettings],
      ['advanced', AdvancedSettings],
    ]);
    for (const [section, component] of settingsSections.entries()) {
      pages.push(m(component, {
        dataSources: [],
        cssClass: maybeGetActiveCss(routePage, section),
      } as RecordingSectionAttrs));
    }

    if (isChromeTarget(globals.state.recordingTarget)) {
      globals.dispatch(Actions.setFetchChromeCategories({fetch: true}));
    }

    return m(
        '.record-page',
        globals.state.recordingInProgress ? m('.hider') : [],
        m('.record-container',
          RecordHeader(),
          m('.record-container-content', recordMenu(routePage), pages)));
  },
});
