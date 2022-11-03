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

import {
  ANDROID_WEBSOCKET_TARGET_FACTORY,
  AndroidWebsocketTargetFactory,
} from
    // clang-format off
'../../common/recordingV2/target_factories/android_websocket_target_factory';
// clang-format on
import {
  targetFactoryRegistry,
} from '../../common/recordingV2/target_factory_registry';

export const FORCE_RESET_MESSAGE = 'Force reset the USB interface';
export const DEFAULT_ADB_WEBSOCKET_URL = 'ws://127.0.0.1:8037/adb';
export const DEFAULT_TRACED_WEBSOCKET_URL = 'ws://127.0.0.1:8037/traced';

export function getWebsocketTargetFactory(): AndroidWebsocketTargetFactory {
  return targetFactoryRegistry.get(ANDROID_WEBSOCKET_TARGET_FACTORY) as
      AndroidWebsocketTargetFactory;
}
