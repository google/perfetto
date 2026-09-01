// Copyright 2026 Comcast Cable Communications Management, LLC
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

import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import {
  getSharedMsgChannelTargetRegistry,
  type MsgChannelTarget,
} from './msgchannel_target_registry';
import {advancedRecordSection} from './pages/advanced';
import {rdkRecordSection} from './pages/rdk';
import {perfettoSDKRecordSection} from './pages/perfetto_sdk';
import {bufferConfigPage} from './pages/buffer_config_page';
import {cpuRecordSection} from './pages/cpu';
import {gpuRecordSection} from './pages/gpu';
import {instructionsPage} from './pages/instructions_page';
import {linuxRecordSection} from './pages/linux';
import {memoryRecordSection} from './pages/memory';
import {powerRecordSection} from './pages/power';
import {RecordPageV2} from './pages/record_page';
import {stackSamplingRecordSection} from './pages/stack_sampling';
import {networkRecordSection} from './pages/network';
import {targetSelectionPage} from './pages/target_selection_page';
import {RecordingManager} from './recording_manager';
import {TracedWebsocketTargetProvider} from './traced_over_websocket/traced_websocket_provider';
import {TracedMsgChannelTargetProvider} from './traced_over_msgchannel/traced_msgchannel_provider';
import m from 'mithril';

export default class implements PerfettoPlugin {
  static readonly id = 'org.rdk.RecordTrace';
  private static recordingMgr?: RecordingManager;
  private static registeredMsgChannelTargets: Array<MsgChannelTarget> = [];

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
      id: 'org.rdk.RecordTrace.disconnectTarget',
      name: 'Disconnect the current device',
      callback: () => {
        const recMgr = this.getRecordingManager(app);
        if (recMgr.currentTarget) {
          recMgr.currentTarget.disconnect();
        }
      },
    });

    // Create a MsgChannelProviderRegistry that installs a listener on
    // window posted events, and if someone has posted us a message channel to
    // use for traced data then we're notified.
    const msgChannelRegistry = getSharedMsgChannelTargetRegistry();
    msgChannelRegistry.onProviderRegistered.addListener((provider) => {
      this.registerMsgChannelTarget(provider);
    });
    for (const provider of msgChannelRegistry.providers) {
      this.registerMsgChannelTarget(provider);
    }
  }

  // Lazily initialize the RecordingManager at first call. This is to prevent
  // providers to connect to sockets / devtools (which in turn can trigger
  // security UX in the browser) before the user has even done anything.
  private static getRecordingManager(app: App): RecordingManager {
    if (this.recordingMgr === undefined) {
      const recMgr = new RecordingManager(app);
      this.recordingMgr = recMgr;

      // Maintain the WebSocket provider in case people want to use that instead
      // with ssh port forwarding or with RDK devices
      recMgr.registerProvider(new TracedWebsocketTargetProvider());

      // Create the MsgChannel provider and register any targets that were
      // registered before the RecordingManager was initialized.
      const msgChannelProvider = new TracedMsgChannelTargetProvider();
      for (const provider of this.registeredMsgChannelTargets) {
        msgChannelProvider.registerTarget(provider);
      }
      recMgr.registerProvider(msgChannelProvider);

      recMgr.registerPage(
        targetSelectionPage(recMgr),
        bufferConfigPage(recMgr),
        instructionsPage(recMgr),

        cpuRecordSection(),
        gpuRecordSection(),
        powerRecordSection(),
        memoryRecordSection(),
        linuxRecordSection(),
        rdkRecordSection(),
        perfettoSDKRecordSection(),
        stackSamplingRecordSection(),
        networkRecordSection(),
        advancedRecordSection(),
      );
      recMgr.restorePluginStateFromLocalstorage();

      // If a MsgChannel target is registered, then default to select that
      // target as the default, regardless of previous settings
      if (msgChannelProvider.targets.size > 0) {
        recMgr.setProvider(msgChannelProvider).catch(console.error);
      }
    }

    // For devtools debugging purposes.
    (window as {} as {recordingMgr: unknown}).recordingMgr = this.recordingMgr;
    return this.recordingMgr;
  }

  // This method is called by external pages that want to register a
  // MessageChannel target
  private static registerMsgChannelTarget(target: MsgChannelTarget) {
    if (this.recordingMgr === undefined) {
      this.registeredMsgChannelTargets.push(target);
    } else {
      const msgChannelProvider = this.recordingMgr.getProvider(
        'traced_msgchannel',
      ) as TracedMsgChannelTargetProvider;
      msgChannelProvider.registerTarget(target);

      // Also if no target is currently selected, then select this one automatically
      if (!this.recordingMgr.currentTarget) {
        this.recordingMgr.setProvider(msgChannelProvider).catch(console.error);
      }
    }
  }
}
