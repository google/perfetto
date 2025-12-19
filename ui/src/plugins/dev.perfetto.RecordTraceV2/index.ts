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
import {perfettoSDKRecordSection} from './pages/perfetto_sdk';
import {bufferConfigPage} from './pages/buffer_config_page';
import {chromeRecordSection} from './pages/chrome';
import {cpuRecordSection} from './pages/cpu';
import {gpuRecordSection} from './pages/gpu';
import {instructionsPage} from './pages/instructions_page';
import {memoryRecordSection} from './pages/memory';
import {powerRecordSection} from './pages/power';
import {RecordPageV2} from './pages/record_page';
import {stackSamplingRecordSection} from './pages/stack_sampling';
import {networkRecordSection} from './pages/network';
import {targetSelectionPage} from './pages/target_selection_page';
import {RecordingManager} from './recording_manager';
import {TracedWebsocketTargetProvider} from './traced_over_websocket/traced_websocket_provider';
import {WebDeviceProxyTargetProvider} from './adb/web_device_proxy/wdp_target_provider';
import m from 'mithril';
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.RecordTraceV2';
  private static recordingMgr?: RecordingManager;

  static onActivate(app: App) {
    app.sidebar.addMenuItem({
      section: 'trace_files',
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
    app.commands.registerCommand({
      id: 'dev.perfetto.RecordTraceV2.disconnectTarget',
      name: 'Disconnect the current device',
      callback: () => {
        const recMgr = this.getRecordingManager(app);
        if (recMgr.currentTarget) {
          recMgr.currentTarget.disconnect();
        }
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

        chromeRecordSection(() => chromeProvider.getChromeCategories()),
        cpuRecordSection(),
        gpuRecordSection(),
        powerRecordSection(),
        memoryRecordSection(),
        androidRecordSection(),
        perfettoSDKRecordSection(),
        stackSamplingRecordSection(),
        networkRecordSection(),
        advancedRecordSection(),
      );
      recMgr.restorePluginStateFromLocalstorage();
    }

    // For devtools debugging purposes.
    (window as {} as {recordingMgr: unknown}).recordingMgr = this.recordingMgr;
    return this.recordingMgr;
  }
}
