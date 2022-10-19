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
  ADB_ENDPOINT,
  DEFAULT_WEBSOCKET_URL,
  TRACED_ENDPOINT,
} from '../../frontend/recording/recording_ui_utils';

import {TargetFactory} from './recording_interfaces_v2';
import {
  ANDROID_WEBSOCKET_TARGET_FACTORY,
  AndroidWebsocketTargetFactory,
} from './target_factories/android_websocket_target_factory';
import {
  HOST_OS_TARGET_FACTORY,
  HostOsTargetFactory,
} from './target_factories/host_os_target_factory';
import {targetFactoryRegistry} from './target_factory_registry';

// The WebsocketMenuController will handle paths for all factories which
// connect over websocket. At present, these are:
// - adb websocket factory
// - host OS websocket factory
export class WebsocketMenuController {
  private path: string = DEFAULT_WEBSOCKET_URL;

  getPath(): string {
    return this.path;
  }

  setPath(path: string): void {
    this.path = path;
  }

  onPathChange(): void {
    if (targetFactoryRegistry.has(ANDROID_WEBSOCKET_TARGET_FACTORY)) {
      const androidTargetFactory =
          targetFactoryRegistry.get(ANDROID_WEBSOCKET_TARGET_FACTORY) as
          AndroidWebsocketTargetFactory;
      androidTargetFactory.tryEstablishWebsocket(this.path + ADB_ENDPOINT);
    }

    if (targetFactoryRegistry.has(HOST_OS_TARGET_FACTORY)) {
      const hostTargetFactory =
          targetFactoryRegistry.get(HOST_OS_TARGET_FACTORY) as
          HostOsTargetFactory;
      hostTargetFactory.tryEstablishWebsocket(this.path + TRACED_ENDPOINT);
    }
  }

  getTargetFactories(): TargetFactory[] {
    const targetFactories = [];
    if (targetFactoryRegistry.has(ANDROID_WEBSOCKET_TARGET_FACTORY)) {
      targetFactories.push(
          targetFactoryRegistry.get(ANDROID_WEBSOCKET_TARGET_FACTORY));
    }
    if (targetFactoryRegistry.has(HOST_OS_TARGET_FACTORY)) {
      targetFactories.push(targetFactoryRegistry.get(HOST_OS_TARGET_FACTORY));
    }
    return targetFactories;
  }
}
