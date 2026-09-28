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

import m from 'mithril';
import {valueIfAllEqual} from '../../base/array_utils';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import type {YMode} from '../../components/tracks/counter_track';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {Form, FormLabel} from '../../widgets/form';
import {MenuItem} from '../../widgets/menu';
import {showModal} from '../../widgets/modal';
import {MultiSelect} from '../../widgets/multiselect';
import {RadioGroup} from '../../widgets/radio_group';
import {Spinner} from '../../widgets/spinner';
import {TextInput} from '../../widgets/text_input';
import {
  createTimeseriesRenderer,
  type TimeseriesCounter,
} from './timeseries_renderer';

// A timeseries track is a track plotting several counters at once.

export interface CounterInfo {
  readonly id: number; // counter_track.id
  readonly name: string;
  readonly unit?: string;
  // The thread or process owning the counter, e.g. "system_server 1719".
  readonly owner?: string;
}

// Whether lines are labelled with the counter's name or its owner's.
type LabelBy = 'name' | 'owner';

export async function listCounters(trace: Trace): Promise<CounterInfo[]> {
  const result = await trace.engine.query(`
    SELECT
      ct.id AS id,
      ct.name AS name,
      ct.unit AS unit,
      thread.tid AS tid,
      thread.name AS threadName,
      process.pid AS pid,
      process.name AS processName
    FROM counter_track ct
    LEFT JOIN thread_counter_track tct ON tct.id = ct.id
    LEFT JOIN thread ON thread.utid = tct.utid
    LEFT JOIN process_counter_track pct ON pct.id = ct.id
    LEFT JOIN process ON process.upid = COALESCE(pct.upid, thread.upid)
    ORDER BY ct.name, process.pid, thread.tid
  `);
  const counters: CounterInfo[] = [];
  const it = result.iter({
    id: NUM,
    name: STR_NULL,
    unit: STR_NULL,
    tid: NUM_NULL,
    threadName: STR_NULL,
    pid: NUM_NULL,
    processName: STR_NULL,
  });
  for (; it.valid(); it.next()) {
    let owner: string | undefined;
    if (it.tid !== null) {
      owner = `${it.threadName ?? 'Thread'} ${it.tid}`;
    } else if (it.pid !== null) {
      owner = `${it.processName ?? 'Process'} ${it.pid}`;
    }
    counters.push({
      id: it.id,
      name: it.name ?? `Counter ${it.id}`,
      unit: it.unit ?? undefined,
      owner,
    });
  }
  return counters;
}

// The counters under the current area selection.
export function selectedCounterIds(trace: Trace): number[] {
  const selection = trace.selection.selection;
  if (selection.kind !== 'area') return [];
  const ids = selection.tracks
    .filter((track) => track.tags?.kinds?.includes(COUNTER_TRACK_KIND))
    .flatMap((track) => track.tags?.trackIds ?? []);
  return [...new Set(ids)];
}

const URI_PREFIX = 'dev.perfetto.TimeseriesTracks#';
let trackCounter = 0;

// Adds a timeseries track to the top of the workspace.
export function addTimeseriesTrack(
  trace: Trace,
  counters: ReadonlyArray<CounterInfo>,
  labelBy = defaultLabelBy(counters),
  title = defaultTitle(counters),
) {
  const uri = `${URI_PREFIX}track${trackCounter++}`;
  const series: TimeseriesCounter[] = counters.map((c) => ({
    id: c.id,
    name: labelBy === 'owner' ? (c.owner ?? c.name) : c.name,
    unit: c.unit,
  }));
  const names = series.map((s) => s.name);
  // Each counter's own track. Timeseries tracks are skipped as one plotting a
  // single counter would otherwise match too.
  const counterTracks = counters.map((counter) =>
    trace.tracks.findTrack(
      (t) =>
        !t.uri.startsWith(URI_PREFIX) &&
        t.tags?.kinds?.includes(COUNTER_TRACK_KIND) &&
        t.tags.trackIds?.length === 1 &&
        t.tags.trackIds[0] === counter.id,
    ),
  );
  trace.tracks.registerTrack({
    uri,
    tags: {
      kinds: [COUNTER_TRACK_KIND],
      trackIds: counters.map((c) => c.id),
    },
    renderer: createTimeseriesRenderer(trace, uri, series, {
      yMode: sharedYMode(counterTracks),
      extraMenuItems: () =>
        m(MenuItem, {
          label: 'Copy to counter tracks',
          icon: 'content_copy',
          disabled: counterTracks.every((t) => t === undefined),
          onclick: () => copyToCounterTracks(trace, uri, counterTracks, names),
        }),
    }),
  });

  // In the timeline like any other track, and pinned, so unpinning leaves it
  // in place rather than removing it.
  const node = new TrackNode({uri, name: title, removable: true});
  trace.currentWorkspace.addChildFirst(node);
  node.pin();
}

// The mode (value, delta or rate) that every counter's own track shows, so the
// chart opens the way they do.
function sharedYMode(
  counterTracks: ReadonlyArray<Track | undefined>,
): YMode | undefined {
  const modes = counterTracks.map(
    (t) =>
      t?.renderer.settings?.find((s) => s.descriptor.name === 'Y Mode')?.value,
  );
  return valueIfAllEqual(modes) as YMode | undefined;
}

// Adds a group holding each counter's own track right after the timeseries
// track, named after it.
function copyToCounterTracks(
  trace: Trace,
  uri: string,
  counterTracks: ReadonlyArray<Track | undefined>,
  names: ReadonlyArray<string>,
) {
  const workspace = trace.currentWorkspace;
  const node =
    workspace.pinnedTracksNode.getTrackByUri(uri) ??
    workspace.getTrackByUri(uri);
  const parent = node?.parent;
  if (node === undefined || parent === undefined) return;

  const group = new TrackNode({
    name: node.name,
    removable: true,
    collapsed: false,
  });
  counterTracks.forEach((track, i) => {
    if (track !== undefined) {
      group.addChildLast(new TrackNode({uri: track.uri, name: names[i]}));
    }
  });
  parent.addChildAfter(group, node);
}

function defaultLabelBy(counters: ReadonlyArray<CounterInfo>): LabelBy {
  const names = new Set(counters.map((c) => c.name));
  return names.size < counters.length ? 'owner' : 'name';
}

function defaultTitle(counters: ReadonlyArray<CounterInfo>): string {
  const names = new Set(counters.map((c) => c.name));
  return names.size === 1 ? counters[0].name : `${counters.length} counters`;
}

// The name a counter is listed and searched by.
function describe(counter: CounterInfo): string {
  return counter.owner === undefined
    ? counter.name
    : `${counter.name} · ${counter.owner}`;
}

export function showTimeseriesTrackDialog(
  trace: Trace,
  selectedIds: ReadonlyArray<number>,
) {
  const dialog = new TimeseriesTrackDialog(trace, selectedIds);
  showModal({
    title: 'Add timeseries track',
    buttons: [
      {
        text: 'Add track',
        primary: true,
        disabled: () => !dialog.canAdd(),
        action: () => dialog.add(),
      },
      {text: 'Cancel'},
    ],
    content: () => dialog.view(),
  });
}

class TimeseriesTrackDialog {
  private counters?: ReadonlyArray<CounterInfo>;
  private readonly selected: Set<number>;
  private labelBy?: LabelBy;
  private title = '';

  constructor(
    private readonly trace: Trace,
    selectedIds: ReadonlyArray<number>,
  ) {
    this.selected = new Set(selectedIds);
    listCounters(trace).then((counters) => {
      this.counters = counters;
      m.redraw();
    });
  }

  canAdd(): boolean {
    return this.selection().length > 0;
  }

  add() {
    const counters = this.selection();
    addTimeseriesTrack(
      this.trace,
      counters,
      this.labelBy ?? defaultLabelBy(counters),
      this.title.trim() || defaultTitle(counters),
    );
  }

  view(): m.Children {
    const counters = this.counters;
    if (counters === undefined) {
      return m(Spinner);
    }
    const selection = this.selection();
    return m(
      Form,
      {className: 'pf-timeseries-track-dialog'},
      m(FormLabel, 'Counters'),
      m(MultiSelect, {
        options: counters.map((counter) => ({
          id: String(counter.id),
          name: describe(counter),
          details: describe(counter),
          checked: this.selected.has(counter.id),
        })),
        repeatCheckedItemsAtTop: true,
        onChange: (diffs) => {
          for (const {id, checked} of diffs) {
            if (checked) {
              this.selected.add(Number(id));
            } else {
              this.selected.delete(Number(id));
            }
          }
        },
      }),
      m(FormLabel, 'Label lines by'),
      m(
        RadioGroup,
        {
          selectedValue: this.labelBy ?? defaultLabelBy(selection),
          onValueChange: (value) => (this.labelBy = value as LabelBy),
        },
        m(RadioGroup.Button, {value: 'name'}, 'Counter name'),
        m(RadioGroup.Button, {value: 'owner'}, 'Process or thread'),
      ),
      m(FormLabel, 'Track name'),
      m(TextInput, {
        value: this.title,
        placeholder: selection.length > 0 ? defaultTitle(selection) : '',
        onInput: (value: string) => (this.title = value),
      }),
    );
  }

  private selection(): CounterInfo[] {
    return (this.counters ?? []).filter((c) => this.selected.has(c.id));
  }
}
