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

import {EXTENSION_NOT_INSTALLED} from '../chrome_utils';
import {RecordingError} from '../recording_error_handling';
import {
  OnTargetChangeCallback,
  RecordingTargetV2,
  TargetFactory,
} from '../recording_interfaces_v2';
import {targetFactoryRegistry} from '../target_factory_registry';
import {ChromeTarget} from '../targets/chrome_target';

const CHROME_TARGET_FACTORY = 'ChromeTargetFactory';

// Sample user agent for Chrome on Chrome OS:
// "Mozilla/5.0 (X11; CrOS x86_64 14816.99.0) AppleWebKit/537.36
// (KHTML, like Gecko) Chrome/103.0.5060.114 Safari/537.36"
// This condition is wider, in the unlikely possibility of different casing,
export function isCrOS(userAgent: string) {
  return userAgent.toLowerCase().includes(' cros ');
}

export class ChromeTargetFactory implements TargetFactory {
  readonly kind = CHROME_TARGET_FACTORY;
  private targets: ChromeTarget[];

  constructor() {
    this.targets = [new ChromeTarget('Chrome', 'CHROME')];
    if (isCrOS(navigator.userAgent)) {
      this.targets.push(new ChromeTarget('Chrome', 'CHROME_OS'));
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
    if (!this.targets[0].getInfo().isExtensionInstalled) {
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
