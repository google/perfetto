// Copyright (C) 2025 The Android Open Source Project
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

import z from 'zod';
import {
  Setting,
  SettingDescriptor,
  SettingsManager,
} from '../../public/settings';

// This class holds all the setting offered by the RecordTraceV2 plugin.
// It is registered at plugin activation time.
class RecordTraceV2Settings {
  static readonly baseId = 'dev.perfetto.RecordTraceV2#';
  static readonly socketAddressDescriptor: SettingDescriptor<string> = {
    id: 'dev.perfetto.RecordTraceV2#socketAddressSetting',
    name: 'Traced socket address',
    description: `The recording plugin communicates with traced using a socket. This setting specifies the address the UI connects to.
    To use a socket in the abstract namespace, prefix its name with "@".`,
    schema: z.string(),
    defaultValue: '/dev/socket/traced_consumer',
    requiresReload: false,
  };
  private socketAddressValue?: Setting<string>;

  // Register all the settings of the RecordTraceV2 plugin
  registerSettings(settingsManager: SettingsManager) {
    this.socketAddressValue = settingsManager.register(
      RecordTraceV2Settings.socketAddressDescriptor,
    );
  }

  // Return the fully formed ADB socket address according to the settings
  // The address is of the form <type>:<address>
  getTracedConsumerSocketAddressForAdb() {
    const address = this.getTracedConsumerSocketAddress();
    if (address.startsWith('@')) {
      return `localabstract:${address.slice(1)}`;
    }
    return `localfilesystem:${address}`;
  }

  // Return the address value in the setting
  getTracedConsumerSocketAddress() {
    if (this.socketAddressValue === undefined) {
      return RecordTraceV2Settings.socketAddressDescriptor.defaultValue;
    }
    return this.socketAddressValue.get();
  }
}

export default new RecordTraceV2Settings();
