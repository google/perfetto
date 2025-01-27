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

import {EvtSource} from '../../../base/events';
import {Result} from '../../../base/result';
import {PreflightCheck} from '../interfaces/connection_check';
import {RecordingTargetProvider} from '../interfaces/recording_target_provider';
import {TargetPlatformId} from '../interfaces/target_platform';
import {ChromeExtensionTarget} from './chrome_extension_target';

export class ChromeExtensionTargetProvider implements RecordingTargetProvider {
  readonly id = 'chrome_extension';
  readonly name = 'Chrome Tracing extension';
  readonly icon = 'extension';
  readonly description = 'Chrome using extension';
  readonly supportedPlatforms = ['CHROME', 'CHROME_OS'] as const;
  readonly onTargetsChanged = new EvtSource<void>();

  private target = new ChromeExtensionTarget();

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {}

  async listTargets(
    platform: TargetPlatformId,
  ): Promise<ChromeExtensionTarget[]> {
    this.target.platform = platform;
    return [this.target];
  }

  getChromeCategories(): Promise<Result<string[]>> {
    return this.target.getChromeCategories();
  }
}
