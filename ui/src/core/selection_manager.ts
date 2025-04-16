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
import {exists} from '../base/utils';
import {TrackManagerImpl} from './track_manager';
import {Engine} from '../trace_processor/engine';
import {ScrollHelper} from './scroll_helper';
import {NoteManagerImpl} from './note_manager';
import {SearchResult} from '../public/search';
import {AsyncLimiter} from '../base/async_limiter';
import m from 'mithril';
import {SerializedSelection} from './state_serialization_schema';
import {showModal} from '../widgets/modal';
import {buildTrackGroups} from './dataset_search';
import {NUM} from '../trace_processor/query_result';

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

  clear(): void {
    this.setSelection({kind: 'empty'});
  }

  async selectTrackEvent(
    trackUri: string,
    eventId: number,
    opts?: SelectionOpts,
  ) {
    this.selectTrackEventInternal(trackUri, eventId, opts);
  }

  selectTrack(trackUri: string, opts?: SelectionOpts) {
    this.setSelection({kind: 'track', trackUri}, opts);
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

  async resolveSqlEvent(
    sqlTableName: string,
    id: number,
  ): Promise<{eventId: number; trackUri: string} | undefined> {
    // Find all the tracks whose root table = sqlTableName
    const tracks = this.trackManager
      .getAllTracks()
      .filter((track) => track.track.rootTableName === sqlTableName);

    const trackGroups = buildTrackGroups(tracks);

    // For each track group, if the base dataset implements {id: NUM}, then we
    // can use it
    for (const [base, group] of trackGroups) {
      if (base.implements({id: NUM})) {
        const partitionCols = Array.from(group.partitioned.keys());
        const query = `
          SELECT
            ${partitionCols.join(',')}
          FROM (${base.query()})
          WHERE id = ${id}
        `;
        const result = await this.engine.query(query);
        const partitionColSchema = Object.fromEntries(
          partitionCols.map((key) => [key, base.schema[key]]),
        );
        const iter = result.iter(partitionColSchema);
        for (; iter.valid(); iter.next()) {
          // Find the track that matches this partition
          // Add results for matching partitioned tracks
          for (const colName of partitionCols) {
            const partitionValue = iter.get(colName);
            const tracks = group.partitioned.get(colName)?.get(partitionValue);

            if (tracks) {
              for (const track of tracks) {
                return {eventId: id, trackUri: track.uri};
              }
            }
          }

          // Add results for non-partitioned tracks (they match any row from the
          // source)
          for (const track of group.nonPartitioned) {
            return {eventId: id, trackUri: track.uri};
          }
        }
      }
    }

    return undefined;
  }

  selectSqlEvent(sqlTableName: string, id: number, opts?: SelectionOpts): void {
    this.resolveSqlEvent(sqlTableName, id).then((selection) => {
      selection &&
        this.selectTrackEvent(selection.trackUri, selection.eventId, opts);
    });
  }

  private setSelection(selection: Selection, opts?: SelectionOpts) {
    this._selection = selection;
    this.onSelectionChange(selection, opts ?? {});

    if (opts?.scrollToSelection) {
      this.scrollToCurrentSelection();
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

  scrollToCurrentSelection() {
    const uri = (() => {
      switch (this.selection.kind) {
        case 'track_event':
        case 'track':
          return this.selection.trackUri;
        // TODO(stevegolton): Handle scrolling to area and note selections.
        default:
          return undefined;
      }
    })();
    const range = this.findTimeRangeOfSelection();
    this.scrollHelper.scrollTo({
      time: range ? {...range} : undefined,
      track: uri ? {uri: uri, expandGroup: true} : undefined,
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

    const trackRenderer = track.track;
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
    const panel = td.track.detailsPanel?.(selection);
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

  findTimeRangeOfSelection(): TimeSpan | undefined {
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
