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

import {TracedWebsocketTarget} from './traced_websocket_target';
import {TracedWebsocketTargetProvider} from './traced_websocket_provider';

/**
 * Shows a dialog that allows to add a connection to another websocket endpoint
 * other than the default 127.0.0.1:8037. This dialog is displayed when the user
 * clicks on "connect new device" in the "Target Device" page.
 */
export async function showTracedConnectionManagementDialog(
  _provider: TracedWebsocketTargetProvider,
): Promise<TracedWebsocketTarget | undefined> {
  // TODO(primiano): implement in next cl.
  return undefined;
}
