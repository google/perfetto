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

import {RecordingError} from '../recording_error_handling';
import {
  OnTargetChangeCallback,
  RecordingTargetV2,
  TargetFactory,
} from '../recording_interfaces_v2';
import {
  EXTENSION_ID,
  EXTENSION_NOT_INSTALLED,
  isCrOS,
} from '../recording_utils';
import {targetFactoryRegistry} from '../target_factory_registry';
import {ChromeTarget} from '../targets/chrome_target';

export const CHROME_TARGET_FACTORY = 'ChromeTargetFactory';

export class ChromeTargetFactory implements TargetFactory {
  readonly kind = CHROME_TARGET_FACTORY;
  // We only check the connection once at the beginning to:
  // a) Avoid creating a 'Port' object every time 'getInfo' is called.
  // b) When a new Port is created, the extension starts communicating with it
  // and leaves aside the old Port objects, so creating a new Port would break
  // any ongoing tracing session.
  isExtensionInstalled: boolean = false;
  private targets: ChromeTarget[] = [];

  constructor() {
    this.init();
  }

  init() {
    const testPort = chrome.runtime.connect(EXTENSION_ID);
    this.isExtensionInstalled = !!testPort;
    testPort.disconnect();

    if (!this.isExtensionInstalled) {
      return;
    }
    this.targets.push(new ChromeTarget('Chrome', 'CHROME'));
    if (isCrOS(navigator.userAgent)) {
      this.targets.push(new ChromeTarget('ChromeOS', 'CHROME_OS'));
    }
  }

  connectNewTarget(): Promise<RecordingTargetV2> {
    throw new RecordingError(
        'Can not create a new Chrome target.' +
        'All Chrome targets are created at factory initialisation.');
  }

  getName(): string {
    return 'Chrome';
  }

  listRecordingProblems(): string[] {
    const recordingProblems = [];
    if (!this.isExtensionInstalled) {
      recordingProblems.push(EXTENSION_NOT_INSTALLED);
    }
    return recordingProblems;
  }

  listTargets(): RecordingTargetV2[] {
    return this.targets;
  }

  setOnTargetChange(onTargetChange: OnTargetChangeCallback): void {
    for (const target of this.targets) {
      target.onTargetChange = onTargetChange;
    }
  }
}

// We only instantiate the factory if Perfetto UI is open in the Chrome browser.
if (window.chrome && chrome.runtime) {
  targetFactoryRegistry.register(new ChromeTargetFactory());
}
