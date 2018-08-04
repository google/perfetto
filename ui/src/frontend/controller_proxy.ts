// Copyright (C) 2018 The Android Open Source Project
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

import {Remote} from '../base/remote';
import {Action} from '../common/actions';
import {State} from '../common/state';

/**
 * Proxy for the Controller worker.
 * This allows us to send strongly typed messages to the contoller.
 * TODO(hjd): Remove the boiler plate.
 */
export class ControllerProxy {
  private readonly remote: Remote;

  constructor(remote: Remote) {
    this.remote = remote;
  }

  initAndGetState(port: MessagePort): Promise<State> {
    return this.remote.send<State>('initAndGetState', [port], [port]);
  }

  dispatch(action: Action): Promise<void> {
    return this.remote.send<void>('dispatch', [action]);
  }
}
