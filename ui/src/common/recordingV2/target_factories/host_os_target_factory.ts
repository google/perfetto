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
import {isLinux, isMacOs} from '../recording_utils';
import {targetFactoryRegistry} from '../target_factory_registry';
import {HostOsTarget} from '../targets/host_os_target';

export const HOST_OS_TARGET_FACTORY = 'HostOsTargetFactory';

export class HostOsTargetFactory implements TargetFactory {
  readonly kind = HOST_OS_TARGET_FACTORY;
  private target?: HostOsTarget;
  private onTargetChange: OnTargetChangeCallback = () => {};

  connectNewTarget(): Promise<RecordingTargetV2> {
    throw new RecordingError(
      'Can not create a new Host OS target.' +
        'The Host OS target is created at factory initialisation.',
    );
  }

  getName(): string {
    return 'HostOs';
  }

  listRecordingProblems(): string[] {
    return [];
  }

  listTargets(): RecordingTargetV2[] {
    if (this.target) {
      return [this.target];
    }
    return [];
  }

  tryEstablishWebsocket(websocketUrl: string) {
    if (this.target) {
      if (this.target.getUrl() === websocketUrl) {
        return;
      } else {
        this.target.disconnect();
      }
    }
    this.target = new HostOsTarget(
      websocketUrl,
      this.maybeClearTarget.bind(this),
      this.onTargetChange,
    );
    this.onTargetChange();
  }

  maybeClearTarget(target: HostOsTarget): void {
    if (this.target === target) {
      this.target = undefined;
      this.onTargetChange();
    }
  }

  setOnTargetChange(onTargetChange: OnTargetChangeCallback): void {
    this.onTargetChange = onTargetChange;
  }
}

// We instantiate the host target factory only on Mac, Linux, and Windows.
if (isMacOs(navigator.userAgent) || isLinux(navigator.userAgent)) {
  targetFactoryRegistry.register(new HostOsTargetFactory());
}
