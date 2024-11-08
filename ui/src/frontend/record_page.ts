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
import {
  getDefaultRecordingTargets,
  hasActiveProbes,
  isAdbTarget,
  isAndroidP,
  isAndroidTarget,
  isChromeTarget,
  isCrOSTarget,
  isLinuxTarget,
  isWindowsTarget,
  LoadedConfig,
  MAX_TIME,
  RecordingTarget,
} from '../common/state';
import {AdbOverWebUsb} from '../controller/adb';
import {RecordConfig} from '../controller/record_config_types';
import {featureFlags} from '../core/feature_flags';
import {raf} from '../core/raf_scheduler';
import {PageAttrs} from '../public/page';
import {
  autosaveConfigStore,
  recordConfigStore,
  recordTargetStore,
} from './record_config';
import {CodeSnippet} from './record_widgets';
import {AdvancedSettings} from './recording/advanced_settings';
import {AndroidSettings} from './recording/android_settings';
import {ChromeSettings} from './recording/chrome_settings';
import {CpuSettings} from './recording/cpu_settings';
import {GpuSettings} from './recording/gpu_settings';
import {LinuxPerfSettings} from './recording/linux_perf_settings';
import {MemorySettings} from './recording/memory_settings';
import {PowerSettings} from './recording/power_settings';
import {RecordingSettings} from './recording/recording_settings';
import {EtwSettings} from './recording/etw_settings';
import {createPermalink} from './permalink';
import {AppImpl} from '../core/app_impl';
import {RecordingManager} from '../controller/recording_manager';

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
  'etw',
  'gpu',
  'power',
  'memory',
  'android',
  'chrome',
  'tracePerf',
  'advanced',
];

function RecordHeader(recMgr: RecordingManager) {
  return m(
    '.record-header',
    m(
      '.top-part',
      m(
        '.target-and-status',
        RecordingPlatformSelection(recMgr),
        RecordingStatusLabel(recMgr),
        ErrorLabel(recMgr),
      ),
      recordingButtons(recMgr),
    ),
    RecordingNotes(recMgr),
  );
}

function RecordingPlatformSelection(recMgr: RecordingManager) {
  if (recMgr.state.recordingInProgress) return [];

  const availableAndroidDevices = recMgr.state.availableAdbDevices;
  const recordingTarget = recMgr.state.recordingTarget;

  const targets = [];
  for (const {os, name} of getDefaultRecordingTargets()) {
    targets.push(m('option', {value: os}, name));
  }
  for (const d of availableAndroidDevices) {
    targets.push(m('option', {value: d.serial}, d.name));
  }

  const selectedIndex = isAdbTarget(recordingTarget)
    ? targets.findIndex((node) => node.attrs.value === recordingTarget.serial)
    : targets.findIndex((node) => node.attrs.value === recordingTarget.os);

  return m(
    '.target',
    m(
      'label',
      'Target platform:',
      m(
        'select',
        {
          selectedIndex,
          onchange: (e: Event) => {
            onTargetChange(recMgr, (e.target as HTMLSelectElement).value);
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
        ...targets,
      ),
    ),
    m(
      '.chip',
      {onclick: () => addAndroidDevice(recMgr)},
      m('button', 'Add ADB Device'),
      m('i.material-icons', 'add'),
    ),
  );
}

// |target| can be the TargetOs or the android serial.
function onTargetChange(recMgr: RecordingManager, target: string) {
  const recordingTarget: RecordingTarget =
    recMgr.state.availableAdbDevices.find((d) => d.serial === target) ||
    getDefaultRecordingTargets().find((t) => t.os === target) ||
    getDefaultRecordingTargets()[0];

  if (isChromeTarget(recordingTarget)) {
    recMgr.setFetchChromeCategories(true);
  }

  recMgr.setRecordingTarget(recordingTarget);
  recordTargetStore.save(target);
  raf.scheduleFullRedraw();
}

function Instructions(recMgr: RecordingManager, cssClass: string) {
  return m(
    `.record-section.instructions${cssClass}`,
    m('header', 'Recording command'),
    PERSIST_CONFIG_FLAG.get()
      ? m(
          'button.permalinkconfig',
          {
            onclick: () => createPermalink({mode: 'RECORDING_OPTS'}),
          },
          'Share recording settings',
        )
      : null,
    RecordingSnippet(recMgr),
    BufferUsageProgressBar(recMgr),
    m('.buttons', StopCancelButtons(recMgr)),
    recordingLog(recMgr),
  );
}

export function loadedConfigEqual(
  cfg1: LoadedConfig,
  cfg2: LoadedConfig,
): boolean {
  return cfg1.type === 'NAMED' && cfg2.type === 'NAMED'
    ? cfg1.name === cfg2.name
    : cfg1.type === cfg2.type;
}

export function loadConfigButton(
  recMgr: RecordingManager,
  config: RecordConfig,
  configType: LoadedConfig,
): m.Vnode {
  return m(
    'button',
    {
      class: 'config-button',
      title: 'Apply configuration settings',
      disabled: loadedConfigEqual(configType, recMgr.state.lastLoadedConfig),
      onclick: () => {
        recMgr.setRecordConfig(config, configType);
        raf.scheduleFullRedraw();
      },
    },
    m('i.material-icons', 'file_upload'),
  );
}

export function displayRecordConfigs(recMgr: RecordingManager) {
  const configs = [];
  if (autosaveConfigStore.hasSavedConfig) {
    configs.push(
      m('.config', [
        m('span.title-config', m('strong', 'Latest started recording')),
        loadConfigButton(recMgr, autosaveConfigStore.get(), {
          type: 'AUTOMATIC',
        }),
      ]),
    );
  }
  for (const item of recordConfigStore.recordConfigs) {
    configs.push(
      m('.config', [
        m('span.title-config', item.title),
        loadConfigButton(recMgr, item.config, {
          type: 'NAMED',
          name: item.title,
        }),
        m(
          'button',
          {
            class: 'config-button',
            title: 'Overwrite configuration with current settings',
            onclick: () => {
              if (
                confirm(
                  `Overwrite config "${item.title}" with current settings?`,
                )
              ) {
                recordConfigStore.overwrite(
                  recMgr.state.recordConfig,
                  item.key,
                );
                recMgr.setRecordConfig(item.config, {
                  type: 'NAMED',
                  name: item.title,
                });
                raf.scheduleFullRedraw();
              }
            },
          },
          m('i.material-icons', 'save'),
        ),
        m(
          'button',
          {
            class: 'config-button',
            title: 'Remove configuration',
            onclick: () => {
              recordConfigStore.delete(item.key);
              raf.scheduleFullRedraw();
            },
          },
          m('i.material-icons', 'delete'),
        ),
      ]),
    );
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

export function Configurations(recMgr: RecordingManager, cssClass: string) {
  const canSave = recordConfigStore.canSave(ConfigTitleState.getTitle());
  return m(
    `.record-section${cssClass}`,
    m('header', 'Save and load configurations'),
    m('.input-config', [
      m('input', {
        value: ConfigTitleState.title,
        placeholder: 'Title for config',
        oninput() {
          ConfigTitleState.setTitle(this.value);
          raf.scheduleFullRedraw();
        },
      }),
      m(
        'button',
        {
          class: 'config-button',
          disabled: !canSave,
          title: canSave
            ? 'Save current config'
            : 'Duplicate name, saving disabled',
          onclick: () => {
            recordConfigStore.save(
              recMgr.state.recordConfig,
              ConfigTitleState.getTitle(),
            );
            raf.scheduleFullRedraw();
            ConfigTitleState.clearTitle();
          },
        },
        m('i.material-icons', 'save'),
      ),
      m(
        'button',
        {
          class: 'config-button',
          title: 'Clear current configuration',
          onclick: () => {
            if (
              confirm(
                'Current configuration will be cleared. ' + 'Are you sure?',
              )
            ) {
              recMgr.clearRecordConfig();
              raf.scheduleFullRedraw();
            }
          },
        },
        m('i.material-icons', 'delete_forever'),
      ),
    ]),
    displayRecordConfigs(recMgr),
  );
}

function BufferUsageProgressBar(recMgr: RecordingManager) {
  if (!recMgr.state.recordingInProgress) return [];

  const bufferUsage = recMgr.state.bufferUsage;
  // Buffer usage is not available yet on Android.
  if (bufferUsage === 0) return [];

  return m(
    'label',
    'Buffer usage: ',
    m('progress', {max: 100, value: bufferUsage * 100}),
  );
}

function RecordingNotes(recMgr: RecordingManager) {
  const sideloadUrl =
    'https://perfetto.dev/docs/contributing/build-instructions#get-the-code';
  const linuxUrl = 'https://perfetto.dev/docs/quickstart/linux-tracing';
  const cmdlineUrl =
    'https://perfetto.dev/docs/quickstart/android-tracing#perfetto-cmdline';
  const extensionURL = `https://chrome.google.com/webstore/detail/perfetto-ui/lfmkphfpdbjijhpomgecfikhfohaoine`;

  const notes: m.Children = [];

  const msgFeatNotSupported = m(
    'span',
    `Some probes are only supported in Perfetto versions running
      on Android Q+. `,
  );

  const msgPerfettoNotSupported = m(
    'span',
    `Perfetto is not supported natively before Android P. `,
  );

  const msgSideload = m(
    'span',
    `If you have a rooted device you can `,
    m(
      'a',
      {href: sideloadUrl, target: '_blank'},
      `sideload the latest version of
         Perfetto.`,
    ),
  );

  const msgRecordingNotSupported = m(
    '.note',
    `Recording Perfetto traces from the UI is not supported natively
     before Android Q. If you are using a P device, please select 'Android P'
     as the 'Target Platform' and `,
    m(
      'a',
      {href: cmdlineUrl, target: '_blank'},
      `collect the trace using ADB.`,
    ),
  );

  const msgChrome = m(
    '.note',
    `To trace Chrome from the Perfetto UI, you need to install our `,
    m('a', {href: extensionURL, target: '_blank'}, 'Chrome extension'),
    ' and then reload this page. ',
  );

  const msgWinEtw = m(
    '.note',
    `To trace with Etw on Windows from the Perfetto UI, you to run chrome with`,
    ` administrator permission and you need to install our `,
    m('a', {href: extensionURL, target: '_blank'}, 'Chrome extension'),
    ' and then reload this page.',
  );

  const msgLinux = m(
    '.note',
    `Use this `,
    m('a', {href: linuxUrl, target: '_blank'}, `quickstart guide`),
    ` to get started with tracing on Linux.`,
  );

  const msgLongTraces = m(
    '.note',
    `Recording in long trace mode through the UI is not supported. Please copy
    the command and `,
    m(
      'a',
      {href: cmdlineUrl, target: '_blank'},
      `collect the trace using ADB.`,
    ),
  );

  const msgZeroProbes = m(
    '.note',
    "It looks like you didn't add any probes. " +
      'Please add at least one to get a non-empty trace.',
  );

  if (!hasActiveProbes(recMgr.state.recordConfig)) {
    notes.push(msgZeroProbes);
  }

  if (isAdbTarget(recMgr.state.recordingTarget)) {
    notes.push(msgRecordingNotSupported);
  }
  switch (recMgr.state.recordingTarget.os) {
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
      if (!recMgr.state.extensionInstalled) notes.push(msgChrome);
      break;
    case 'CrOS':
      if (!recMgr.state.extensionInstalled) notes.push(msgChrome);
      break;
    case 'Win':
      if (!recMgr.state.extensionInstalled) notes.push(msgWinEtw);
      break;
    default:
  }
  if (recMgr.state.recordConfig.mode === 'LONG_TRACE') {
    notes.unshift(msgLongTraces);
  }

  return notes.length > 0 ? m('div', notes) : [];
}

function RecordingSnippet(recMgr: RecordingManager) {
  const target = recMgr.state.recordingTarget;

  // We don't need commands to start tracing on chrome
  if (isChromeTarget(target)) {
    return recMgr.state.extensionInstalled && !recMgr.state.recordingInProgress
      ? m(
          'div',
          m(
            'label',
            `To trace Chrome from the Perfetto UI you just have to press
         'Start Recording'.`,
          ),
        )
      : [];
  }
  return m(CodeSnippet, {text: getRecordCommand(recMgr, target)});
}

function getRecordCommand(recMgr: RecordingManager, target: RecordingTarget) {
  const data = recMgr.state.recordCmd;

  const cfg = recMgr.state.recordConfig;
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
    cmd += isAndroidTarget(target)
      ? 'adb shell perfetto \\\n'
      : 'perfetto \\\n';
    cmd += '  -c - --txt \\\n';
    cmd += '  -o /data/misc/perfetto-traces/trace \\\n';
    cmd += '<<EOF\n\n';
    cmd += pbtx;
    cmd += '\nEOF\n';
  }
  return cmd;
}

function recordingButtons(recMgr: RecordingManager) {
  const state = recMgr.state;
  const target = state.recordingTarget;
  const recInProgress = state.recordingInProgress;

  const start = m(
    `button`,
    {
      class: recInProgress ? '' : 'selected',
      onclick: () => onStartRecordingPressed(recMgr),
    },
    'Start Recording',
  );

  const buttons: m.Children = [];

  if (isAndroidTarget(target)) {
    if (
      !recInProgress &&
      isAdbTarget(target) &&
      recMgr.state.recordConfig.mode !== 'LONG_TRACE'
    ) {
      buttons.push(start);
    }
  } else if (
    (isWindowsTarget(target) || isChromeTarget(target)) &&
    state.extensionInstalled
  ) {
    buttons.push(start);
  }
  return m('.button', buttons);
}

function StopCancelButtons(recMgr: RecordingManager) {
  if (!recMgr.state.recordingInProgress) return [];

  const stop = m(
    `button.selected`,
    {onclick: () => recMgr.stopRecording()},
    'Stop',
  );

  const cancel = m(
    `button`,
    {onclick: () => recMgr.cancelRecording()},
    'Cancel',
  );

  return [stop, cancel];
}

function onStartRecordingPressed(recMgr: RecordingManager) {
  location.href = '#!/record/instructions';
  raf.scheduleFullRedraw();
  autosaveConfigStore.save(recMgr.state.recordConfig);

  const target = recMgr.state.recordingTarget;
  if (
    isAndroidTarget(target) ||
    isChromeTarget(target) ||
    isWindowsTarget(target)
  ) {
    AppImpl.instance.analytics.logEvent(
      'Record Trace',
      `Record trace (${target.os})`,
    );
    recMgr.startRecording();
  }
}

function RecordingStatusLabel(recMgr: RecordingManager) {
  const recordingStatus = recMgr.state.recordingStatus;
  if (!recordingStatus) return [];
  return m('label', recordingStatus);
}

export function ErrorLabel(recMgr: RecordingManager) {
  const lastRecordingError = recMgr.state.lastRecordingError;
  if (!lastRecordingError) return [];
  return m('label.error-label', `Error:  ${lastRecordingError}`);
}

function recordingLog(recMgr: RecordingManager) {
  const logs = recMgr.state.recordingLog;
  if (logs === undefined) return [];
  return m('.code-snippet.no-top-bar', m('code', logs));
}

// The connection must be done in the frontend. After it, the serial ID will
// be inserted in the state, and the worker will be able to connect to the
// correct device.
async function addAndroidDevice(recMgr: RecordingManager) {
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
  await recMgr.updateAvailableAdbDevices(device.serialNumber);
}

function recordMenu(recMgr: RecordingManager, routePage: string) {
  const target = recMgr.state.recordingTarget;
  const chromeProbe = m(
    'a[href="#!/record/chrome"]',
    m(
      `li${routePage === 'chrome' ? '.active' : ''}`,
      m('i.material-icons', 'laptop_chromebook'),
      m('.title', 'Chrome'),
      m('.sub', 'Chrome traces'),
    ),
  );
  const cpuProbe = m(
    'a[href="#!/record/cpu"]',
    m(
      `li${routePage === 'cpu' ? '.active' : ''}`,
      m('i.material-icons', 'subtitles'),
      m('.title', 'CPU'),
      m('.sub', 'CPU usage, scheduling, wakeups'),
    ),
  );
  const gpuProbe = m(
    'a[href="#!/record/gpu"]',
    m(
      `li${routePage === 'gpu' ? '.active' : ''}`,
      m('i.material-icons', 'aspect_ratio'),
      m('.title', 'GPU'),
      m('.sub', 'GPU frequency, memory'),
    ),
  );
  const powerProbe = m(
    'a[href="#!/record/power"]',
    m(
      `li${routePage === 'power' ? '.active' : ''}`,
      m('i.material-icons', 'battery_charging_full'),
      m('.title', 'Power'),
      m('.sub', 'Battery and other energy counters'),
    ),
  );
  const memoryProbe = m(
    'a[href="#!/record/memory"]',
    m(
      `li${routePage === 'memory' ? '.active' : ''}`,
      m('i.material-icons', 'memory'),
      m('.title', 'Memory'),
      m('.sub', 'Physical mem, VM, LMK'),
    ),
  );
  const androidProbe = m(
    'a[href="#!/record/android"]',
    m(
      `li${routePage === 'android' ? '.active' : ''}`,
      m('i.material-icons', 'android'),
      m('.title', 'Android apps & svcs'),
      m('.sub', 'atrace and logcat'),
    ),
  );
  const advancedProbe = m(
    'a[href="#!/record/advanced"]',
    m(
      `li${routePage === 'advanced' ? '.active' : ''}`,
      m('i.material-icons', 'settings'),
      m('.title', 'Advanced settings'),
      m('.sub', 'Complicated stuff for wizards'),
    ),
  );
  const tracePerfProbe = m(
    'a[href="#!/record/tracePerf"]',
    m(
      `li${routePage === 'tracePerf' ? '.active' : ''}`,
      m('i.material-icons', 'full_stacked_bar_chart'),
      m('.title', 'Stack Samples'),
      m('.sub', 'Lightweight stack polling'),
    ),
  );
  const etwProbe = m(
    'a[href="#!/record/etw"]',
    m(
      `li${routePage === 'etw' ? '.active' : ''}`,
      m('i.material-icons', 'subtitles'),
      m('.title', 'ETW Tracing Config'),
      m('.sub', 'Context switch, Thread state'),
    ),
  );
  const recInProgress = recMgr.state.recordingInProgress;

  const probes = [];
  if (isLinuxTarget(target)) {
    probes.push(cpuProbe, powerProbe, memoryProbe, chromeProbe, advancedProbe);
  } else if (isChromeTarget(target) && !isCrOSTarget(target)) {
    probes.push(chromeProbe);
  } else if (isWindowsTarget(target)) {
    probes.push(chromeProbe, etwProbe);
  } else {
    probes.push(
      cpuProbe,
      gpuProbe,
      powerProbe,
      memoryProbe,
      androidProbe,
      chromeProbe,
      tracePerfProbe,
      advancedProbe,
    );
  }

  return m(
    '.record-menu',
    {
      class: recInProgress ? 'disabled' : '',
      onclick: () => raf.scheduleFullRedraw(),
    },
    m('header', 'Trace config'),
    m(
      'ul',
      m(
        'a[href="#!/record/buffers"]',
        m(
          `li${routePage === 'buffers' ? '.active' : ''}`,
          m('i.material-icons', 'tune'),
          m('.title', 'Recording settings'),
          m('.sub', 'Buffer mode, size and duration'),
        ),
      ),
      m(
        'a[href="#!/record/instructions"]',
        m(
          `li${routePage === 'instructions' ? '.active' : ''}`,
          m('i.material-icons-filled.rec', 'fiber_manual_record'),
          m('.title', 'Recording command'),
          m('.sub', 'Manually record trace'),
        ),
      ),
      PERSIST_CONFIG_FLAG.get()
        ? m(
            'a[href="#!/record/config"]',
            {
              onclick: () => {
                recordConfigStore.reloadFromLocalStorage();
              },
            },
            m(
              `li${routePage === 'config' ? '.active' : ''}`,
              m('i.material-icons', 'save'),
              m('.title', 'Saved configs'),
              m('.sub', 'Manage local configs'),
            ),
          )
        : null,
    ),
    m('header', 'Probes'),
    m('ul', probes),
  );
}

export function maybeGetActiveCss(routePage: string, section: string): string {
  return routePage === section ? '.active' : '';
}

export class RecordPage implements m.ClassComponent<PageAttrs> {
  private readonly recMgr = RecordingManager.instance;

  view({attrs}: m.CVnode<PageAttrs>) {
    const pages: m.Children = [];
    // we need to remove the `/` character from the route
    let routePage = attrs.subpage ? attrs.subpage.substr(1) : '';
    if (!RECORDING_SECTIONS.includes(routePage)) {
      routePage = 'buffers';
    }
    pages.push(recordMenu(this.recMgr, routePage));

    pages.push(
      m(RecordingSettings, {
        dataSources: [],
        cssClass: maybeGetActiveCss(routePage, 'buffers'),
        recState: this.recMgr.state,
      }),
    );
    pages.push(
      Instructions(this.recMgr, maybeGetActiveCss(routePage, 'instructions')),
    );
    pages.push(
      Configurations(this.recMgr, maybeGetActiveCss(routePage, 'config')),
    );

    const settingsSections = new Map([
      ['cpu', CpuSettings],
      ['gpu', GpuSettings],
      ['power', PowerSettings],
      ['memory', MemorySettings],
      ['android', AndroidSettings],
      ['chrome', ChromeSettings],
      ['tracePerf', LinuxPerfSettings],
      ['advanced', AdvancedSettings],
      ['etw', EtwSettings],
    ]);
    for (const [section, component] of settingsSections.entries()) {
      pages.push(
        m(component, {
          dataSources: [],
          cssClass: maybeGetActiveCss(routePage, section),
          recState: this.recMgr.state,
        }),
      );
    }

    if (isChromeTarget(this.recMgr.state.recordingTarget)) {
      this.recMgr.setFetchChromeCategories(true);
    }

    return m(
      '.record-page',
      this.recMgr.state.recordingInProgress ? m('.hider') : [],
      m(
        '.record-container',
        RecordHeader(this.recMgr),
        m(
          '.record-container-content',
          recordMenu(this.recMgr, routePage),
          pages,
        ),
      ),
    );
  }
}
