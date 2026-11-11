// Copyright (C) 2026 The Android Open Source Project
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

import './styles.scss';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {
  addTimeseriesTrack,
  listCounters,
  selectedCounterIds,
  showTimeseriesTrackDialog,
} from './timeseries_track';

// Timeseries tracks plot several counters on a single track.
export default class TimeseriesTracksPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TimeseriesTracks';
  static readonly description =
    'Adds commands to plot several counters on a single timeseries track.';

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.AddTimeseriesTrack',
      name: 'Add timeseries track',
      callback: () => showTimeseriesTrackDialog(ctx, selectedCounterIds(ctx)),
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AddTimeseriesTrackFromSelection',
      name: 'Add timeseries track from selected counter tracks',
      callback: async () => {
        const ids = new Set(selectedCounterIds(ctx));
        const counters = (await listCounters(ctx)).filter((c) => ids.has(c.id));
        if (counters.length === 0) {
          ctx.omnibox.showStatusMessage('No counter tracks are selected');
          return;
        }
        addTimeseriesTrack(ctx, counters);
      },
    });
  }
}
