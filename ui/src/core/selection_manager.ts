// Copyright (C) 2024 The Android Open Source Project
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

import {assertTrue, assertUnreachable} from '../base/logging';
import {
  Selection,
  Area,
  SelectionOpts,
  SelectionManager,
  TrackEventSelection,
  AreaSelectionTab,
} from '../public/selection';
import {TimeSpan} from '../base/time';
import {raf} from './raf_scheduler';
import {exists, getOrCreate} from '../base/utils';
import {TrackManagerImpl} from './track_manager';
import {Engine} from '../trace_processor/engine';
import {ScrollHelper} from './scroll_helper';
import {NoteManagerImpl} from './note_manager';
import {SearchResult} from '../public/search';
import {AsyncLimiter} from '../base/async_limiter';
import m from 'mithril';
import {SerializedSelection} from './state_serialization_schema';
import {showModal} from '../widgets/modal';
import {NUM, SqlValue, UNKNOWN} from '../trace_processor/query_result';
import {UnionDataset, SourceDataset} from '../trace_processor/dataset';
import {Track} from '../public/track';

interface SelectionDetailsPanel {
  isLoading: boolean;
  render(): m.Children;
  serializatonState(): unknown;
}

// There are two selection-related states in this class.
// 1. _selection: This is the "input" / locator of the selection, what other
//    parts of the codebase specify (e.g., a tuple of trackUri + eventId) to say
//    "please select this object if it exists".
// 2. _selected{Slice,ThreadState}: This is the resolved selection, that is, the
//    rich details about the object that has been selected. If the input
//    `_selection` is valid, this is filled in the near future. Doing so
//    requires querying the SQL engine, which is an async operation.
export class SelectionManagerImpl implements SelectionManager {
  private readonly detailsPanelLimiter = new AsyncLimiter();
  private _selection: Selection = {kind: 'empty'};
  private readonly detailsPanels = new WeakMap<
    Selection,
    SelectionDetailsPanel
  >();
  public readonly areaSelectionTabs: AreaSelectionTab[] = [];

  constructor(
    private readonly engine: Engine,
    private trackManager: TrackManagerImpl,
    private noteManager: NoteManagerImpl,
    private scrollHelper: ScrollHelper,
    private onSelectionChange: (s: Selection, opts: SelectionOpts) => void,
  ) {}

  clearSelection(): void {
    this.setSelection({kind: 'empty'});
  }

  async selectTrackEvent(
    trackUri: string,
    eventId: number,
    opts?: SelectionOpts,
  ) {
    this.selectTrackEventInternal(trackUri, eventId, opts);
  }

  selectTrack(uri: string, opts?: SelectionOpts) {
    this.setSelection({kind: 'track', trackUri: uri}, opts);
  }

  selectNote(args: {id: string}, opts?: SelectionOpts) {
    this.setSelection(
      {
        kind: 'note',
        id: args.id,
      },
      opts,
    );
  }

  selectArea(area: Area, opts?: SelectionOpts): void {
    const {start, end} = area;
    assertTrue(start <= end);

    // In the case of area selection, the caller provides a list of trackUris.
    // However, all the consumers want to access the resolved Tracks. Rather
    // than delegating this to the various consumers, we resolve them now once
    // and for all and place them in the selection object.
    const tracks = [];
    for (const uri of area.trackUris) {
      const trackDescr = this.trackManager.getTrack(uri);
      if (trackDescr === undefined) continue;
      tracks.push(trackDescr);
    }

    this.setSelection(
      {
        ...area,
        kind: 'area',
        tracks,
      },
      opts,
    );
  }

  deserialize(serialized: SerializedSelection | undefined) {
    if (serialized === undefined) {
      return;
    }
    this.deserializeInternal(serialized);
  }

  private async deserializeInternal(serialized: SerializedSelection) {
    try {
      switch (serialized.kind) {
        case 'TRACK_EVENT':
          await this.selectTrackEventInternal(
            serialized.trackKey,
            parseInt(serialized.eventId),
            undefined,
            serialized.detailsPanel,
          );
          break;
        case 'AREA':
          this.selectArea({
            start: serialized.start,
            end: serialized.end,
            trackUris: serialized.trackUris,
          });
      }
    } catch (ex) {
      showModal({
        title: 'Failed to restore the selected event',
        content: m(
          'div',
          m(
            'p',
            `Due to a version skew between the version of the UI the trace was
             shared with and the version of the UI you are using, we were
             unable to restore the selected event.`,
          ),
          m(
            'p',
            `These backwards incompatible changes are very rare but is in some
             cases unavoidable. We apologise for the inconvenience.`,
          ),
        ),
        buttons: [
          {
            text: 'Continue',
            primary: true,
          },
        ],
      });
    }
  }

  toggleTrackAreaSelection(trackUri: string) {
    const curSelection = this._selection;
    if (curSelection.kind !== 'area') return;

    let trackUris = curSelection.trackUris.slice();
    if (!trackUris.includes(trackUri)) {
      trackUris.push(trackUri);
    } else {
      trackUris = trackUris.filter((t) => t !== trackUri);
    }
    this.selectArea({
      ...curSelection,
      trackUris,
    });
  }

  toggleGroupAreaSelection(trackUris: string[]) {
    const curSelection = this._selection;
    if (curSelection.kind !== 'area') return;

    const allTracksSelected = trackUris.every((t) =>
      curSelection.trackUris.includes(t),
    );

    let newTrackUris: string[];
    if (allTracksSelected) {
      // Deselect all tracks in the list
      newTrackUris = curSelection.trackUris.filter(
        (t) => !trackUris.includes(t),
      );
    } else {
      newTrackUris = curSelection.trackUris.slice();
      trackUris.forEach((t) => {
        if (!newTrackUris.includes(t)) {
          newTrackUris.push(t);
        }
      });
    }
    this.selectArea({
      ...curSelection,
      trackUris: newTrackUris,
    });
  }

  get selection(): Selection {
    return this._selection;
  }

  getDetailsPanelForSelection(): SelectionDetailsPanel | undefined {
    return this.detailsPanels.get(this._selection);
  }

  async resolveSqlEvents(
    sqlTableName: string,
    ids: ReadonlyArray<number>,
  ): Promise<ReadonlyArray<{eventId: number; trackUri: string}>> {
    // This function:
    // 1. Find the list of tracks whose rootTableName is the same as the one we
    //    are looking for
    // 2. Groups them by their filter column - i.e. utid, cpu, or track_id.
    // 3. Builds a map of which of these column values match which track.
    // 4. Run one query per group, reading out the filter column value, and
    //    looking up the originating track in the map.
    // One flaw of this approach is that.
    const groups = new Map<string, [SourceDataset, Track][]>();
    const tracksWithNoFilter: [SourceDataset, Track][] = [];
    const matches: {eventId: number; trackUri: string}[] = [];

    this.trackManager
      .getAllTracks()
      .filter((track) => track.renderer.rootTableName === sqlTableName)
      .map((track) => {
        const dataset = track.renderer.getDataset?.();
        if (!dataset) return undefined;
        return [dataset, track] as const;
      })
      .filter(exists)
      .filter(([dataset]) => dataset.implements({id: NUM}))
      .forEach(([dataset, track]) => {
        const col = dataset.filter?.col;
        if (col) {
          const existingGroup = getOrCreate(groups, col, () => []);
          existingGroup.push([dataset, track]);
        } else {
          tracksWithNoFilter.push([dataset, track]);
        }
      });

    // Run one query per no-filter track. This is the only way we can reliably
    // keep track of which track the event belonged to.
    for (const [dataset, track] of tracksWithNoFilter) {
      const query = `select id from (${dataset.query()}) where id IN (${ids.join(',')})`;
      const result = await this.engine.query(query);
      if (result.numRows() > 0) {
        matches.push({
          eventId: result.firstRow({id: NUM}).id,
          trackUri: track.uri,
        });
      }
    }

    for (const [colName, values] of groups) {
      // Build a map of the values -> track uri
      const map = new Map<SqlValue, string>();
      values.forEach(([dataset, track]) => {
        const filter = dataset.filter;
        if (filter) {
          if ('eq' in filter) map.set(filter.eq, track.uri);
          if ('in' in filter) filter.in.forEach((v) => map.set(v, track.uri));
        }
      });

      const datasets = values.map(([dataset]) => dataset);
      const union = UnionDataset.create(datasets);

      // Make sure to include the filter value in the schema.
      const schema = {...union.schema, [colName]: UNKNOWN};
      const query = `select * from (${union.query(schema)}) where id IN (${ids.join(',')})`;
      const result = await this.engine.query(query);

      const getTrackFromFilterValue = function (value: SqlValue) {
        let trackUri = map.get(value);

        // If that didn't work, try converting the value to a number if it's a
        // bigint. Unless specified as a NUM type, any integers on the wire will
        // be parsed as a bigint to avoid losing precision.
        if (trackUri === undefined && typeof value === 'bigint') {
          trackUri = map.get(Number(value));
        }
        return trackUri;
      };

      const row = result.iter(schema);
      for (; row.valid(); row.next()) {
        const value = row.get(colName);
        const trackUri = getTrackFromFilterValue(value);
        if (trackUri) {
          matches.push({eventId: row.id as number, trackUri});
        }
      }
    }

    return matches;
  }

  async resolveSqlEvent(
    sqlTableName: string,
    id: number,
  ): Promise<{eventId: number; trackUri: string} | undefined> {
    const matches = await this.resolveSqlEvents(sqlTableName, [id]);
    return matches[0];
  }

  async selectSqlEvent(sqlTableName: string, id: number, opts?: SelectionOpts) {
    const event = await this.resolveSqlEvent(sqlTableName, id);
    event && this.selectTrackEvent(event.trackUri, event.eventId, opts);
  }

  private setSelection(selection: Selection, opts?: SelectionOpts) {
    this._selection = selection;
    this.onSelectionChange(selection, opts ?? {});

    if (opts?.scrollToSelection) {
      this.scrollToSelection();
    }
  }

  selectSearchResult(searchResult: SearchResult) {
    const {source, eventId, trackUri} = searchResult;
    if (eventId === undefined) {
      return;
    }
    switch (source) {
      case 'track':
        this.selectTrack(trackUri, {
          clearSearch: false,
          scrollToSelection: true,
        });
        break;
      case 'cpu':
        this.selectSqlEvent('sched_slice', eventId, {
          clearSearch: false,
          scrollToSelection: true,
          switchToCurrentSelectionTab: true,
        });
        break;
      case 'log':
        this.selectSqlEvent('android_logs', eventId, {
          clearSearch: false,
          scrollToSelection: true,
          switchToCurrentSelectionTab: true,
        });
        break;
      case 'slice':
        // Search results only include slices from the slice table for now.
        // When we include annotations we need to pass the correct table.
        this.selectSqlEvent('slice', eventId, {
          clearSearch: false,
          scrollToSelection: true,
          switchToCurrentSelectionTab: true,
        });
        break;
      case 'event':
        this.selectTrackEvent(trackUri, eventId, {
          clearSearch: false,
          scrollToSelection: true,
          switchToCurrentSelectionTab: true,
        });
        break;
      default:
        assertUnreachable(source);
    }
  }

  scrollToSelection(behavior?: 'pan' | 'focus') {
    const uri = (() => {
      switch (this.selection.kind) {
        case 'track_event':
        case 'track':
          return this.selection.trackUri;
        case 'area':
          // For area selections, scroll to the top track
          return this.selection.trackUris.length > 0
            ? this.selection.trackUris[0]
            : undefined;
        case 'note':
          // Notes have no associated track, so only scroll horizontally
          return undefined;
        case 'empty':
          return undefined;
        default:
          return undefined;
      }
    })();
    const range = this.getTimeSpanOfSelection();
    // Note: DEFAULT notes return a TimeSpan with start === end (duration 0),
    // so they're handled as instant events in the scroll helper.
    this.scrollHelper.scrollTo({
      time: range ? {...range, behavior} : undefined,
      track: uri ? {uri, expandGroup: true} : undefined,
    });
  }

  private async selectTrackEventInternal(
    trackUri: string,
    eventId: number,
    opts?: SelectionOpts,
    serializedDetailsPanel?: unknown,
  ) {
    const track = this.trackManager.getTrack(trackUri);
    if (!track) {
      throw new Error(
        `Unable to resolve selection details: Track ${trackUri} not found`,
      );
    }

    const trackRenderer = track.renderer;
    if (!trackRenderer.getSelectionDetails) {
      throw new Error(
        `Unable to resolve selection details: Track ${trackUri} does not support selection details`,
      );
    }

    const details = await trackRenderer.getSelectionDetails(eventId);
    if (!exists(details)) {
      throw new Error(
        `Unable to resolve selection details: Track ${trackUri} returned no details for event ${eventId}`,
      );
    }

    const selection: TrackEventSelection = {
      ...details,
      kind: 'track_event',
      trackUri,
      eventId,
    };
    this.createTrackEventDetailsPanel(selection, serializedDetailsPanel);
    this.setSelection(selection, opts);
  }

  private createTrackEventDetailsPanel(
    selection: TrackEventSelection,
    serializedState: unknown,
  ) {
    const td = this.trackManager.getTrack(selection.trackUri);
    if (!td) {
      return;
    }
    const panel = td.renderer.detailsPanel?.(selection);
    if (!panel) {
      return;
    }

    if (panel.serialization && serializedState !== undefined) {
      const res = panel.serialization.schema.safeParse(serializedState);
      if (res.success) {
        panel.serialization.state = res.data;
      }
    }

    const detailsPanel: SelectionDetailsPanel = {
      render: () => panel.render(),
      serializatonState: () => panel.serialization?.state,
      isLoading: true,
    };
    // Associate this details panel with this selection object
    this.detailsPanels.set(selection, detailsPanel);

    this.detailsPanelLimiter.schedule(async () => {
      await panel?.load?.(selection);
      detailsPanel.isLoading = false;
      raf.scheduleFullRedraw();
    });
  }

  getTimeSpanOfSelection(): TimeSpan | undefined {
    const sel = this.selection;
    if (sel.kind === 'area') {
      return new TimeSpan(sel.start, sel.end);
    } else if (sel.kind === 'note') {
      const selectedNote = this.noteManager.getNote(sel.id);
      if (selectedNote !== undefined) {
        const kind = selectedNote.noteType;
        switch (kind) {
          case 'SPAN':
            return new TimeSpan(selectedNote.start, selectedNote.end);
          case 'DEFAULT':
            // A TimeSpan where start === end is treated as an instant event.
            return new TimeSpan(selectedNote.timestamp, selectedNote.timestamp);
          default:
            assertUnreachable(kind);
        }
      }
    } else if (sel.kind === 'track_event') {
      switch (sel.dur) {
        case undefined:
        case -1n:
          // Events without a duration or with duration -1 (DNF) slices are just
          // treated as if they were instant events.
          return TimeSpan.fromTimeAndDuration(sel.ts, 0n);
        default:
          return TimeSpan.fromTimeAndDuration(sel.ts, sel.dur);
      }
    }

    return undefined;
  }

  registerAreaSelectionTab(tab: AreaSelectionTab): void {
    this.areaSelectionTabs.push(tab);
  }
}
