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
  TrackControllerFactory,
  trackControllerRegistry,
} from '../controller/track_controller';
import {TrackCreator} from '../frontend/track';
import {trackRegistry} from '../frontend/track_registry';

import {PluginContext, PluginInfo} from './plugin_api';
import {Registry} from './registry';

// Every plugin gets its own PluginContext. This is how we keep track
// what each plugin is doing and how we can blame issues on particular
// plugins.
export class PluginContextImpl implements PluginContext {
  pluginName: string;

  constructor(pluginName: string) {
    this.pluginName = pluginName;
  }

  registerTrackController(track: TrackControllerFactory): void {
    trackControllerRegistry.register(track);
  }

  registerTrack(track: TrackCreator): void {
    trackRegistry.register(track);
  }
}

export const pluginRegistry = new Registry<PluginInfo>((info) => {
  return info.pluginId;
});
