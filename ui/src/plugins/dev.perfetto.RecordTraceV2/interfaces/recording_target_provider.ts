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

import {Evt} from '../../../base/events';
import {PreflightCheck, WithPreflightChecks} from './connection_check';
import {RecordingTarget} from './recording_target';
import {TargetPlatformId} from './target_platform';

/**
 * The interface to describe target providers. A target provider uses a specific
 * transport (e.g., WebUsb, WebSocket, Chrome extension) and allows to find
 * and obtain Targets.
 */
export interface RecordingTargetProvider extends WithPreflightChecks {
  readonly id: string;
  readonly name: string;
  readonly icon: string;
  readonly description: string;
  readonly supportedPlatforms: ReadonlyArray<TargetPlatformId>;

  /**
   * Event listener raised when the target list changes.
   * The caller is expected to call listTargets() in response to this.
   */
  readonly onTargetsChanged: Evt<void>;

  /** Returns a list of debugging checks to diagnose connection failures. */
  runPreflightChecks(): AsyncGenerator<PreflightCheck>;

  /**
   * Lists the targets that can be discovered. Note that some providers
   * (notably WebUSB) can't discover devices that never got paired before and
   * need a call to {@link pairNewTarget()} to pop up a pair dialog.
   */
  listTargets(platform: TargetPlatformId): Promise<RecordingTarget[]>;

  /**
   * Optional. Some transports can't discover all targets upfront and need
   * some user interaction to add a new target.
   */
  pairNewTarget?: () => Promise<RecordingTarget | undefined>;
}
