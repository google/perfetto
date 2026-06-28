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

import {EvtSource} from '../../../base/events';
import type {PreflightCheck} from '../interfaces/connection_check';
import type {RecordingTargetProvider} from '../interfaces/recording_target_provider';
import {TracedMsgChannelTarget} from './traced_msgchannel_target';
import type {MsgChannelTarget} from '../msgchannel_target_registry';

export class TracedMsgChannelTargetProvider implements RecordingTargetProvider {
  readonly id = 'traced_msgchannel';
  readonly name = 'MessageChannel';
  readonly description =
    'Allows to talk to the traced service via a MessageChannel proxy. ' +
    'Requires being launched from another webpage that provides the MessageChannel';
  readonly icon = 'swap_horiz';
  readonly supportedPlatforms = ['LINUX'] as const;
  readonly onTargetsChanged = new EvtSource<void>();

  readonly targets = new Map<string, TracedMsgChannelTarget>();

  constructor() {}

  registerTarget(target: MsgChannelTarget) {
    this.targets.set(
      target.session,
      new TracedMsgChannelTarget(
        target.srcWindow,
        target.srcDomain,
        target.session,
      ),
    );
    this.onTargetsChanged.notify();
  }

  async listTargets(): Promise<TracedMsgChannelTarget[]> {
    return Array.from(this.targets.values());
  }

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {}
}
