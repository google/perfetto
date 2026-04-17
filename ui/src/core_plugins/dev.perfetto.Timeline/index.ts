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

import m from 'mithril';
import z from 'zod';
import {AppImpl} from '../../core/app_impl';
import {TraceImpl} from '../../core/trace_impl';
import {Flag} from '../../public/feature_flag';
import {PerfettoPlugin} from '../../public/plugin';
import {Setting} from '../../public/settings';
import {TimelinePage} from './timeline_page';

const DEFAULT_TRACK_MIN_HEIGHT_PX = 18;
const MINIMUM_TRACK_MIN_HEIGHT_PX = 18;

export default class TimelinePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Timeline';
  static readonly description = 'The main timeline view';
  private static minimapFlag: Flag;
  private static minTrackHeightSetting: Setting<number>;

  static onActivate(app: AppImpl): void {
    // This setting is referenced in the track view by name
    TimelinePlugin.minTrackHeightSetting = app.settings.register({
      id: 'dev.perfetto.TrackMinHeightPx',
      name: 'Track Height',
      description:
        'Minimum height of tracks in the trace viewer page, in pixels.',
      schema: z.number().int().min(MINIMUM_TRACK_MIN_HEIGHT_PX),
      defaultValue: DEFAULT_TRACK_MIN_HEIGHT_PX,
    });

    TimelinePlugin.minimapFlag = app.featureFlags.register({
      id: 'overviewVisible',
      name: 'Overview Panel',
      description: 'Show the panel providing an overview of the trace',
      defaultValue: true,
    });
  }

  async onTraceLoad(trace: TraceImpl): Promise<void> {
    trace.pages.registerPage({
      route: '/viewer',
      render: () => {
        return m(TimelinePage, {
          trace,
          showMinimap: TimelinePlugin.minimapFlag.get(),
          minTrackHeight: TimelinePlugin.minTrackHeightSetting.get(),
        });
      },
    });
  }
}
