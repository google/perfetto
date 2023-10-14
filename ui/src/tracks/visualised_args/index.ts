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

import m from 'mithril';

import {Actions} from '../../common/actions';
import {globals} from '../../frontend/globals';
import {NewTrackArgs, TrackBase} from '../../frontend/track';
import {TrackButton, TrackButtonAttrs} from '../../frontend/track_panel';
import {Plugin, PluginContext, PluginDescriptor} from '../../public';
import {
  ChromeSliceTrack,
  ChromeSliceTrackController,
  Config as ChromeSliceConfig,
} from '../chrome_slices';

export {Data} from '../chrome_slices';

export const VISUALISED_ARGS_SLICE_TRACK_KIND = 'VisualisedArgsTrack';

export interface Config extends ChromeSliceConfig {
  argName: string;
}

// The controller for arg visualisation is exactly the same as the controller
// for Chrome slices. All customisation is done on the frontend.
class VisualisedArgsTrackController extends ChromeSliceTrackController {
  static readonly kind = VISUALISED_ARGS_SLICE_TRACK_KIND;
}

export class VisualisedArgsTrack extends ChromeSliceTrack {
  static readonly kind = VISUALISED_ARGS_SLICE_TRACK_KIND;
  static create(args: NewTrackArgs): TrackBase {
    return new VisualisedArgsTrack(args);
  }

  getFont() {
    return 'italic 11px Roboto';
  }

  getTrackShellButtons(): m.Children {
    const config = this.config as Config;
    const buttons: Array<m.Vnode<TrackButtonAttrs>> = [];
    buttons.push(m(TrackButton, {
      action: () => {
        globals.dispatch(
            Actions.removeVisualisedArg({argName: config.argName}));
      },
      i: 'close',
      tooltip: 'Close',
      showButton: true,
    }));
    return buttons;
  }
}

class VisualisedArgsPlugin implements Plugin {
  onActivate(ctx: PluginContext): void {
    ctx.LEGACY_registerTrackController(VisualisedArgsTrackController);
    ctx.LEGACY_registerTrack(VisualisedArgsTrack);
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.VisualisedArgs',
  plugin: VisualisedArgsPlugin,
};
