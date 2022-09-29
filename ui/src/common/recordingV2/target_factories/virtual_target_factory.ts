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
import {targetFactoryRegistry} from '../target_factory_registry';
import {AndroidVirtualTarget} from '../targets/android_virtual_target';

const VIRTUAL_TARGET_FACTORY = 'VirtualTargetFactory';

export class VirtualTargetFactory implements TargetFactory {
  readonly kind: string = VIRTUAL_TARGET_FACTORY;
  private targets: AndroidVirtualTarget[];

  constructor() {
    this.targets = [];
    this.targets.push(new AndroidVirtualTarget('Android Q', 29));
    this.targets.push(new AndroidVirtualTarget('Android P', 28));
    this.targets.push(new AndroidVirtualTarget('Android O-', 27));
  }

  connectNewTarget(): Promise<RecordingTargetV2> {
    throw new RecordingError(
        'Can not create a new virtual target.' +
        'All virtual targets are created at factory initialisation.');
  }

  getName(): string {
    return 'Virtual';
  }

  listRecordingProblems(): string[] {
    return [];
  }

  listTargets(): RecordingTargetV2[] {
    return this.targets;
  }

  // Virtual targets won't change.
  setOnTargetChange(_: OnTargetChangeCallback): void {}
}

targetFactoryRegistry.register(new VirtualTargetFactory());
