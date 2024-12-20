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
import {PreflightCheck} from '../interfaces/connection_check';
import {RecordingTarget} from '../interfaces/recording_target';
import {RecordingTargetProvider} from '../interfaces/recording_target_provider';
import {showTracedConnectionManagementDialog} from './target_connection_management_dialog';
import {TracedWebsocketTarget} from './traced_websocket_target';

export class TracedWebsocketTargetProvider implements RecordingTargetProvider {
  readonly id = 'traced_websocket';
  readonly name = 'WebSocket';
  readonly description =
    'Allows to talk to the traced service UNIX socket via a WebSocket. ' +
    'Requires launching the websocket_bridge on the host';
  readonly icon = 'lan';
  readonly supportedPlatforms = ['LINUX'] as const;
  readonly onTargetsChanged = new EvtSource<void>();

  readonly targets = new Map<string, TracedWebsocketTarget>();

  constructor() {
    // Add the default target.
    const defaultWsUrl = 'ws://127.0.0.1:8037/traced';
    this.targets.set(defaultWsUrl, new TracedWebsocketTarget(defaultWsUrl));
  }

  async listTargets(): Promise<TracedWebsocketTarget[]> {
    return Array.from(this.targets.values());
  }

  pairNewTarget(): Promise<RecordingTarget | undefined> {
    return showTracedConnectionManagementDialog(this);
  }

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {}
}
