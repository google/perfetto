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

import './timeseries_track.scss';
import m from 'mithril';
import {valueIfAllEqual} from '../../base/array_utils';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {Form, FormLabel} from '../../widgets/form';
import {MenuItem} from '../../widgets/menu';
import {showModal} from '../../widgets/modal';
import {MultiSelect} from '../../widgets/multiselect';
import {RadioGroup} from '../../widgets/radio_group';
import {Spinner} from '../../widgets/spinner';
import {TextInput} from '../../widgets/text_input';
import {type CounterDisplaySettings, CounterTrack} from './counter_track';

// A timeseries track is a counter track plotting several counters at once.

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

let trackCounter = 0;

// Adds a timeseries track to the top of the workspace. It holds the counters'
// own tracks as children, so expanding it shows them as separate tracks and
// collapsing it shows them as one again.
export function addTimeseriesTrack(
  trace: Trace,
  counters: ReadonlyArray<CounterInfo>,
  labelBy = defaultLabelBy(counters),
  title = defaultTitle(counters),
) {
  const uri = `timeseries.track${trackCounter++}`;
  const names = counters.map((c) =>
    labelBy === 'owner' ? (c.owner ?? c.name) : c.name,
  );
  const counterTracks = counters.map((counter) =>
    trace.tracks.findTrack(
      (t) =>
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
    renderer: CounterTrack.create({
      trace,
      uri,
      series: counters.map((c, i) => ({
        name: names[i],
        sqlSource: `SELECT ts, value FROM counter WHERE track_id = ${c.id}`,
        unit: c.unit,
      })),
      chartHeightSize: 4,
      ...sharedDisplaySettings(counterTracks),
      menuItems: () => {
        const workspace = trace.currentWorkspace;
        const node =
          workspace.pinnedTracksNode.getTrackByUri(uri) ??
          workspace.getTrackByUri(uri);
        if (!node?.hasChildren) return undefined;
        return m(MenuItem, {
          label: node.expanded
            ? 'Show as one track'
            : 'Show as separate tracks',
          icon: node.expanded ? 'call_merge' : 'call_split',
          onclick: () => node.toggleCollapsed(),
        });
      },
    }),
  });

  const node = new TrackNode({
    uri,
    name: title,
    removable: true,
    isSummary: true,
  });
  counterTracks.forEach((track, i) => {
    if (track !== undefined) {
      node.addChildLast(new TrackNode({uri: track.uri, name: names[i]}));
    }
  });
  trace.currentWorkspace.pinnedTracksNode.addChildLast(node);
}

// The display settings that all of the counters' own tracks agree on, so the
// chart opens the way they are shown.
function sharedDisplaySettings(
  tracks: ReadonlyArray<Track | undefined>,
): CounterDisplaySettings {
  const settings = tracks
    .map((t) => t?.renderer)
    .filter((r): r is CounterTrack => r instanceof CounterTrack)
    .map((r) => r.displaySettings);
  const agreed = <K extends keyof CounterDisplaySettings>(key: K) =>
    valueIfAllEqual(settings.map((s) => s[key]));
  return {
    yMode: agreed('yMode'),
    yRange: agreed('yRange'),
    yDisplay: agreed('yDisplay'),
    yRangeRounding: agreed('yRangeRounding'),
  };
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
