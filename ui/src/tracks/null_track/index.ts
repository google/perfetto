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

import {PluginContext} from '../../common/plugin_api';
import {NewTrackArgs, Track} from '../../frontend/track';

export const NULL_TRACK_KIND = 'NullTrack';

export class NullTrack extends Track {
  static readonly kind = NULL_TRACK_KIND;
  constructor(args: NewTrackArgs) {
    super(args);
    this.frontendOnly = true;
  }

  static create(args: NewTrackArgs): NullTrack {
    return new NullTrack(args);
  }

  getHeight(): number {
    return 30;
  }

  renderCanvas(_: CanvasRenderingContext2D): void {}
}

function activate(ctx: PluginContext) {
  ctx.registerTrack(NullTrack);
}

export const plugin = {
  pluginId: 'perfetto.NullTrack',
  activate,
};
