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

import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {AdbWebsocketTargetProvider} from './adb/websocket/adb_websocket_target_provider';
import {AdbWebusbTargetProvider} from './adb/webusb/adb_webusb_target_provider';
import {ChromeExtensionTargetProvider} from './chrome/chrome_extension_target_provider';
import {advancedRecordSection} from './pages/advanced';
import {androidRecordSection} from './pages/android';
import {bufferConfigPage} from './pages/buffer_config_page';
import {chromeRecordSection} from './pages/chrome';
import {instructionsPage} from './pages/instructions_page';
import {cpuRecordSection} from './pages/cpu';
import {gpuRecordSection} from './pages/gpu';
import {memoryRecordSection} from './pages/memory';
import {powerRecordSection} from './pages/power';
import {RecordPageV2} from './pages/record_page';
import {stackSamplingRecordSection} from './pages/stack_sampling';
import {targetSelectionPage} from './pages/target_selection_page';
import {RecordingManager} from './recording_manager';
import {TracedWebsocketTargetProvider} from './traced_over_websocket/traced_websocket_provider';
import {savedConfigsPage} from './pages/saved_configs';
import {WebDeviceProxyTargetProvider} from './adb/web_device_proxy/wdp_target_provider';
import m from 'mithril';
import {RecordSubpage} from './config/config_interfaces';
import {
  DataGrid,
  renderCell,
} from '../../components/widgets/data_grid/data_grid';
import {Stack, StackAuto} from '../../widgets/stack';
import {Button, ButtonBar, ButtonVariant} from '../../widgets/button';
import {RowDef} from '../../components/widgets/data_grid/common';
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.RecordTraceV2';
  private static recordingMgr?: RecordingManager;

  static onActivate(app: App) {
    app.sidebar.addMenuItem({
      section: 'navigation',
      text: 'Record new trace',
      href: '#!/record',
      icon: 'fiber_smart_record',
      sortOrder: 2,
    });
    app.pages.registerPage({
      route: '/record',
      render: (subpage) => {
        return m(RecordPageV2, {
          subpage,
          app,
          getRecordingManager: () => this.getRecordingManager(app),
        });
      },
    });
  }

  // Lazily initialize the RecordingManager at first call. This is to prevent
  // providers to connect to sockets / devtools (which in turn can trigger
  // security UX in the browser) before the user has even done anything.
  private static getRecordingManager(app: App): RecordingManager {
    if (this.recordingMgr === undefined) {
      const recMgr = new RecordingManager(app);
      this.recordingMgr = recMgr;
      recMgr.registerProvider(new AdbWebusbTargetProvider());
      recMgr.registerProvider(new AdbWebsocketTargetProvider());
      recMgr.registerProvider(new WebDeviceProxyTargetProvider());

      const chromeProvider = new ChromeExtensionTargetProvider();
      recMgr.registerProvider(chromeProvider);
      recMgr.registerProvider(new TracedWebsocketTargetProvider());
      recMgr.registerPage(
        targetSelectionPage(recMgr),
        bufferConfigPage(recMgr),
        instructionsPage(recMgr),
        savedConfigsPage(recMgr),

        chromeRecordSection(() => chromeProvider.getChromeCategories()),
        cpuRecordSection(),
        gpuRecordSection(),
        powerRecordSection(),
        memoryRecordSection(),
        androidRecordSection(),
        stackSamplingRecordSection(),
        advancedRecordSection(),

        myNewPage(),
      );
      recMgr.restorePluginStateFromLocalstorage();
    }

    // For devtools debugging purposes.
    (window as {} as {recordingMgr: unknown}).recordingMgr = this.recordingMgr;
    return this.recordingMgr;
  }
}

function myNewPage(): RecordSubpage {
  return {
    kind: 'GLOBAL_PAGE',
    id: 'liveViewer',
    title: 'System Monitor',
    subtitle: 'Live process data from the device',
    icon: 'browse_activity',
    render: () => {
      return m(SystemMonitor);
    },
    serialize: () => {
      // No state to save.
    },
    deserialize: () => {
      // No state to restore.
    },
  };
}

function formatBytes(bytes: number, decimals = 2) {
  if (bytes === 0) {
    return '0 Bytes';
  }
  const k = 1024;
  const dm = decimals < 0 ? 0 : decimals;
  const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
}

class SystemMonitor implements m.ClassComponent {
  private interval?: ReturnType<typeof setInterval>;
  private readonly procs = [
    'Chrome',
    'Firefox',
    'Google Backup Transport',
    'Google Backup Transport',
    'Google One Backup',
    'Google Play Services',
    'Google Play Store',
    'Google Services Framework',
    'Instagram',
    'logcat',
    'Perfetto',
    'Settings',
    'SurfaceFlinger',
    'SysUI',
    'tachyon',
    'traced_probes',
    'traced',
    'Whatsapp',
    'Youtube',
  ];
  private rows?: RowDef[] = [];

  oninit() {
    this.randomizeValues();
  }

  view() {
    return m('.pf-live-viewer-page', [
      m(Stack, [
        m(
          ButtonBar,
          m(Button, {
            label: 'Capture heap dump',
            variant: ButtonVariant.Filled,
          }),
          m(Button, {
            label: 'Start heap profile',
            variant: ButtonVariant.Filled,
          }),
        ),
        m(DataGrid, {
          fillHeight: true,
          columns: [
            {name: 'proc', title: 'Process'},
            {name: 'mem', title: 'Mem'},
            {name: 'mem.rss', title: 'RSS'},
            {name: 'mem.anon', title: 'Anon'},
            {name: 'mem.file', title: 'File'},
            {name: 'mem.shmem', title: 'Shmem'},
          ],
          data: this.rows!,
          cellRenderer: (value, columnName) => {
            if (columnName.startsWith('mem')) {
              const bytes = Number(value);
              if (isNaN(bytes)) {
                return m('span.pf-data-grid__cell--number', `${value}`);
              }
              return m('span.pf-data-grid__cell--number', formatBytes(bytes));
            } else if (columnName === 'proc') {
              return m(
                Stack,
                {
                  orientation: 'horizontal',
                },
                m(StackAuto, `${value}`),
                m(Button, {
                  className: 'pf-visible-on-hover',
                  compact: true,
                  variant: ButtonVariant.Filled,
                  label: 'Profile',
                }),
                m(Button, {
                  className: 'pf-visible-on-hover',
                  compact: true,
                  variant: ButtonVariant.Filled,
                  label: 'Dump',
                }),
              );
            } else {
              return renderCell(value, columnName);
            }
          },
        }),
      ]),
    ]);
  }

  oncreate() {
    this.interval = setInterval(() => {
      this.randomizeValues();
      m.redraw();
    }, 3000);
  }

  onremove() {
    clearInterval(this.interval);
  }

  private randomizeValues() {
    this.rows = this.procs.map((p) => ({
      'proc': p,
      'mem': Math.random() * 1000000,
      'mem.rss': Math.random() * 1000000,
      'mem.anon': Math.random() * 1000000,
      'mem.file': Math.random() * 1000000,
      'mem.shmem': Math.random() * 1000000,
    }));
  }
}
