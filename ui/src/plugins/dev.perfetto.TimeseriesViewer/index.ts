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
import m from 'mithril';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {ViewerModel} from './model';
import {TimeseriesView} from './timeseries_view';

const TAB_URI = 'dev.perfetto.TimeseriesViewer#dashboard';

// Counter track ids in an area selection (their COUNTER_TRACK_KIND + trackIds,
// deduped) — the same recognition the built-in counter aggregator uses.
function selectedCounterIds(
  tracks: ReadonlyArray<{
    tags?: {kinds?: ReadonlyArray<string>; trackIds?: ReadonlyArray<number>};
  }>,
): number[] {
  return [
    ...new Set(
      tracks
        .filter((t) => t.tags?.kinds?.includes(COUNTER_TRACK_KIND))
        .flatMap((t) => t.tags?.trackIds ?? []),
    ),
  ];
}

// A high-performance, Battery-Historian-style viewer for exploring counters
// over long (24h–weeks) traces, embedded in the timeline drawer so counters can
// be related to the tracks above. It appears as a timeline-synced drawer tab —
// openable from a command (then add counters via the picker) or contextually by
// selecting counter tracks (or whole groups) in the timeline.
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TimeseriesViewer';
  static readonly description =
    'Fast multi-counter timeseries viewer in the timeline drawer (zoom/pan synced, per-lane auto-scaled axes)';

  async onTraceLoad(trace: Trace): Promise<void> {
    // One shared model, created lazily on first use. Both entry points below
    // render it, so the counter set and view state stay consistent.
    let model: ViewerModel | undefined;
    const getModel = (): ViewerModel => {
      if (model === undefined) {
        model = new ViewerModel(trace);
        void model.init().then(() => m.redraw());
      }
      return model;
    };

    // The viewer as a persistent drawer tab, opened by the command below
    // even with nothing selected (add counters from its picker).
    trace.tabs.registerTab({
      uri: TAB_URI,
      content: {
        getTitle: () => 'Timeseries',
        render: () => m(TimeseriesView, {model: getModel()}),
      },
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.TimeseriesViewer#open',
      name: 'Open Timeseries viewer',
      callback: () => {
        // Always open empty — add counters from the picker (the contextual
        // path below is for plotting an existing timeline selection).
        getModel().deselectAll();
        trace.tabs.showTab(TAB_URI);
      },
    });

    // Contextual: selecting counter tracks (or a group) shows the same
    // viewer inline in the Current Selection drawer, tracking the selection
    // (manual picker adds are preserved alongside).
    trace.selection.registerAreaSelectionTab({
      id: 'dev.perfetto.TimeseriesViewer',
      name: 'Timeseries',
      priority: 50,
      render(selection) {
        const ids = selectedCounterIds(selection.tracks);
        if (ids.length === 0) return undefined;
        const mdl = getModel();
        if (!mdl.loading) mdl.setAreaSelection(ids);
        return {isLoading: false, content: m(TimeseriesView, {model: mdl})};
      },
    });
  }
}
