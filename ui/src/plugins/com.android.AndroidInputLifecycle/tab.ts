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
import {Time, Duration, duration, time} from '../../base/time';
import {Trace} from '../../public/trace';
import {DetailsShell} from '../../widgets/details_shell';
import {Grid, GridHeaderCell, GridCell, GridColumn} from '../../widgets/grid';
import {EmptyState} from '../../widgets/empty_state';
import {Spinner} from '../../widgets/spinner';
import {
  NUM,
  STR,
  LONG,
  LONG_NULL,
  NUM_NULL,
  UNKNOWN,
} from '../../trace_processor/query_result';
import {Dataset, UnionDatasetWithLineage} from '../../trace_processor/dataset';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {Checkbox} from '../../widgets/checkbox';
import {LifecycleOverlay} from './overlay';
import {ArrowConnection} from './arrow_visualiser';
import {DurationWidget} from '../../components/widgets/duration';
import {Tab} from '../../public/tab';

// --- Interfaces ---

interface NavTarget {
  id: number;
  trackId: number;
  trackUri: string;
  ts: time;
  dur: duration;
  depth: number;
}

interface InputChainRow {
  uiRowId: string;
  channel: string;
  totalLatency: duration | null;

  // Latency Deltas
  durReader: duration | null;
  deltaDispatch: duration | null;
  deltaReceive: duration | null;
  deltaConsume: duration | null;
  deltaFrame: duration | null;

  navReader?: NavTarget;
  navDispatch?: NavTarget;
  navConsume?: NavTarget;
  navReceive?: NavTarget;
  navFrame?: NavTarget;

  allTrackIds: number[];
  input_id_val: string;
}

function getTrackUriForTrackId(trace: Trace, trackId: number): string {
  const track = trace.tracks.findTrack((t) =>
    t.tags?.trackIds?.includes(trackId),
  );
  return track?.uri || `/slice_${trackId}`;
}

const RELATION_SCHEMA = {
  id: NUM,
  name: STR,
  ts: LONG,
  dur: LONG,
  track_id: NUM,
  depth: NUM,
};

export class AndroidInputTab implements Tab {
  private rows: InputChainRow[] = [];
  private visibleRowIds = new Set<string>();
  private pinnedRowIds = new Set<string>();
  private currentSelectionId?: number;
  private isLoading = false;

  constructor(
    private trace: Trace,
    private overlay: LifecycleOverlay,
  ) {}

  onHide() {
    this.overlay.update([]);
    this.rows = [];
    this.visibleRowIds.clear();
    this.pinnedRowIds.clear();
    this.currentSelectionId = undefined;
    this.isLoading = false;
  }

  getTitle() {
    return 'Android Input Lifecycle';
  }

  private syncSelection() {
    const selection = this.trace.selection.selection;
    if (selection.kind !== 'track_event') return;
    if (selection.eventId === this.currentSelectionId) return;

    this.currentSelectionId = selection.eventId;
    this.loadData(selection.eventId);
  }

  private async loadData(clickedEventId: number) {
    this.isLoading = true;

    const query = `
      SELECT * FROM _android_input_lifecycle_by_slice_id(${clickedEventId})
    `;

    try {
      const result = await this.trace.engine.query(query);

      const it = result.iter({
        input_id: STR,
        channel: STR,
        total_latency: LONG_NULL,

        ts_reader: LONG_NULL,
        ts_dispatch: LONG_NULL,
        ts_receive: LONG_NULL,
        ts_consume: LONG_NULL,
        ts_frame: LONG_NULL,

        id_reader: NUM_NULL,
        track_reader: NUM_NULL,
        dur_reader: LONG_NULL,
        id_dispatch: NUM_NULL,
        track_dispatch: NUM_NULL,
        dur_dispatch: LONG_NULL,
        id_receive: NUM_NULL,
        track_receive: NUM_NULL,
        dur_receive: LONG_NULL,
        id_consume: NUM_NULL,
        track_consume: NUM_NULL,
        dur_consume: LONG_NULL,
        id_frame: NUM_NULL,
        track_frame: NUM_NULL,
        dur_frame: LONG_NULL,
      });

      this.rows = [];
      this.visibleRowIds.clear();
      this.pinnedRowIds.clear();

      let index = 0;
      let rowToHighlight: string | undefined;

      while (it.valid()) {
        const uniqueId = `row-${index++}`;

        // 1. Create Nav Targets
        const navReader = this.makeNav(
          it.id_reader,
          it.track_reader,
          it.ts_reader,
          it.dur_reader,
        );
        const navDispatch = this.makeNav(
          it.id_dispatch,
          it.track_dispatch,
          it.ts_dispatch,
          it.dur_dispatch,
        );
        const navReceive = this.makeNav(
          it.id_receive,
          it.track_receive,
          it.ts_receive,
          it.dur_receive,
        );
        const navConsume = this.makeNav(
          it.id_consume,
          it.track_consume,
          it.ts_consume,
          it.dur_consume,
        );
        const navFrame = this.makeNav(
          it.id_frame,
          it.track_frame,
          it.ts_frame,
          it.dur_frame,
        );

        // 2. Calculate Deltas
        const durReader =
          it.dur_reader !== null ? Duration.fromRaw(it.dur_reader) : null;
        const deltaDispatch =
          it.ts_dispatch !== null && it.ts_reader !== null
            ? Duration.fromRaw(it.ts_dispatch - it.ts_reader)
            : null;
        const deltaReceive =
          it.ts_receive !== null && it.ts_dispatch !== null
            ? Duration.fromRaw(it.ts_receive - it.ts_dispatch)
            : null;
        const deltaConsume =
          it.ts_consume !== null && it.ts_receive !== null
            ? Duration.fromRaw(it.ts_consume - it.ts_receive)
            : null;
        const deltaFrame =
          it.ts_frame !== null && it.ts_consume !== null
            ? Duration.fromRaw(it.ts_frame - it.ts_consume)
            : null;

        const tracks = new Set<number>();
        [navReader, navDispatch, navConsume, navReceive, navFrame].forEach(
          (n) => {
            if (n) tracks.add(n.trackId);
          },
        );

        // 3. Check if this row matches the clicked event
        const matchesClickedEvent = [
          it.id_reader,
          it.id_dispatch,
          it.id_receive,
          it.id_consume,
          it.id_frame,
        ].includes(clickedEventId);

        if (matchesClickedEvent && rowToHighlight === undefined) {
          rowToHighlight = uniqueId;
        }

        this.rows.push({
          uiRowId: uniqueId,
          channel: it.channel,
          totalLatency:
            it.total_latency !== null
              ? Duration.fromRaw(it.total_latency)
              : null,
          durReader,
          deltaDispatch,
          deltaReceive,
          deltaConsume,
          deltaFrame,
          navReader,
          navDispatch,
          navConsume,
          navReceive,
          navFrame,
          allTrackIds: Array.from(tracks),
          input_id_val: it.input_id,
        });

        it.next();
      }

      if (rowToHighlight) {
        this.visibleRowIds.add(rowToHighlight);
      }

      this.updateWorkspacePinning();

      if (this.rows.length > 0) {
        await this.enrichDepths();
      }
      this.updateOverlay();
    } finally {
      this.isLoading = false;
    }
  }

  private makeNav(
    id: number | null,
    trackId: number | null,
    ts: bigint | null,
    dur: bigint | null,
  ): NavTarget | undefined {
    if (id === null || trackId === null || ts === null) return undefined;
    return {
      id,
      trackId,
      trackUri: getTrackUriForTrackId(this.trace, trackId),
      ts: Time.fromRaw(ts),
      dur: Duration.fromRaw(dur ?? 0n),
      depth: 0,
    };
  }

  private async enrichDepths(): Promise<void> {
    const trackIds = new Set<number>();
    const eventIds = new Set<number>();
    const nodeMap = new Map<number, NavTarget[]>();

    for (const row of this.rows) {
      [
        row.navReader,
        row.navDispatch,
        row.navConsume,
        row.navReceive,
        row.navFrame,
      ].forEach((nav) => {
        if (!nav) return;
        trackIds.add(nav.trackId);
        eventIds.add(nav.id);
        if (!nodeMap.has(nav.id)) nodeMap.set(nav.id, []);
        nodeMap.get(nav.id)!.push(nav);
      });
    }

    const trackDatasets: Dataset[] = [];
    for (const trackId of trackIds) {
      const trackUri = getTrackUriForTrackId(this.trace, trackId);
      const track = this.trace.tracks.getTrack(trackUri);
      if (track?.renderer?.getDataset) {
        const ds = track.renderer.getDataset();
        if (ds) trackDatasets.push(ds);
      }
    }

    if (trackDatasets.length === 0) return;

    const unionDataset = UnionDatasetWithLineage.create(trackDatasets);
    const idsArray = Array.from(eventIds);
    const querySchema = {
      ...RELATION_SCHEMA,
      __groupid: NUM,
      __partition: UNKNOWN,
    };

    const sql = `SELECT * FROM (${unionDataset.query(querySchema)}) WHERE id IN (${idsArray.join(',')})`;

    try {
      const result = await this.trace.engine.query(sql);
      const it = result.iter(querySchema);
      while (it.valid()) {
        const nodes = nodeMap.get(it.id);
        if (nodes) {
          nodes.forEach((n) => (n.depth = Number(it.depth)));
        }
        it.next();
      }
    } catch (e) {
      console.error(`Error fetching depths:`, e);
    }
  }

  private toggleVisibility(rowId: string) {
    if (this.visibleRowIds.has(rowId)) this.visibleRowIds.delete(rowId);
    else this.visibleRowIds.add(rowId);
    this.updateOverlay();
  }

  private toggleAllVisibility() {
    const allVisible = this.rows.every((r) =>
      this.visibleRowIds.has(r.uiRowId),
    );
    if (allVisible) this.visibleRowIds.clear();
    else this.rows.forEach((r) => this.visibleRowIds.add(r.uiRowId));
    this.updateOverlay();
  }

  private togglePinning(row: InputChainRow) {
    if (this.pinnedRowIds.has(row.uiRowId)) {
      this.pinnedRowIds.delete(row.uiRowId);
    } else this.pinnedRowIds.add(row.uiRowId);
    this.updateWorkspacePinning();
  }

  private updateWorkspacePinning() {
    const tracksToPin = new Set<number>();
    this.rows.forEach((row) => {
      if (this.pinnedRowIds.has(row.uiRowId)) {
        row.allTrackIds.forEach((tid) => tracksToPin.add(tid));
      }
    });

    const allManagedTracks = new Set<number>();
    this.rows.forEach((row) =>
      row.allTrackIds.forEach((tid) => allManagedTracks.add(tid)),
    );

    this.trace.currentWorkspace.flatTracks.forEach((trackNode) => {
      if (!trackNode.uri) return;
      const descriptor = this.trace.tracks.getTrack(trackNode.uri);
      const trackSqlIds = descriptor?.tags?.trackIds;
      if (!trackSqlIds || trackSqlIds.length === 0) return;

      const isManaged = trackSqlIds.some((id) => allManagedTracks.has(id));
      if (!isManaged) return;

      const shouldBePinned = trackSqlIds.some((id) => tracksToPin.has(id));
      if (shouldBePinned && !trackNode.isPinned) trackNode.pin();
      else if (!shouldBePinned && trackNode.isPinned) trackNode.unpin();
    });
  }

  private updateOverlay() {
    const arrows: ArrowConnection[] = [];
    const visibleRows = this.rows.filter((r) =>
      this.visibleRowIds.has(r.uiRowId),
    );

    for (const row of visibleRows) {
      const steps = [
        row.navReader,
        row.navDispatch,
        row.navReceive,
        row.navConsume,
        row.navFrame,
      ];
      const presentSteps = steps.filter((s): s is NavTarget => s !== undefined);

      for (let i = 0; i < presentSteps.length - 1; i++) {
        const start = presentSteps[i];
        const end = presentSteps[i + 1];
        arrows.push({
          start: {
            trackUri: start.trackUri,
            ts: Time.add(start.ts, start.dur),
            depth: start.depth,
          },
          end: {trackUri: end.trackUri, ts: end.ts, depth: end.depth},
        });
      }
    }
    this.overlay.update(arrows);
  }

  private goTo(nav?: NavTarget) {
    if (!nav) return;
    this.trace.selection.selectTrackEvent(nav.trackUri, nav.id, {
      scrollToSelection: true,
      switchToCurrentSelectionTab: false,
    });
  }

  render(): m.Children {
    this.syncSelection();

    if (this.isLoading) {
      return m(
        DetailsShell,
        {title: this.getTitle()},
        m(
          'div',
          {style: {display: 'flex', justifyContent: 'center', padding: '20px'}},
          m(Spinner, {}),
        ),
      );
    }

    const allVisible = this.rows.every((r) =>
      this.visibleRowIds.has(r.uiRowId),
    );

    const columns: GridColumn[] = [
      {
        key: 'show',
        widthPx: 40,
        header: m(
          GridHeaderCell,
          {},
          m(Checkbox, {
            checked: allVisible,
            onchange: () => this.toggleAllVisibility(),
          }),
        ),
      },
      {
        key: 'pin',
        widthPx: 40,
        header: m(GridHeaderCell, {}, 'Pin'),
      },
      {
        key: 'chan',
        header: m(GridHeaderCell, {}, 'Channel'),
      },
      {
        key: 'total',
        minWidthPx: 100,
        header: m(GridHeaderCell, {}, 'Total Latency'),
      },
      {
        key: 'read',
        minWidthPx: 100,
        header: m(GridHeaderCell, {}, 'InputReader'),
      },
      {
        key: 'disp',
        minWidthPx: 100,
        header: m(GridHeaderCell, {}, 'Dispatcher'),
      },
      {
        key: 'recv',
        minWidthPx: 100,
        header: m(GridHeaderCell, {}, 'App Receive'),
      },
      {
        key: 'cons',
        minWidthPx: 100,
        header: m(GridHeaderCell, {}, 'App Consume'),
      },
      {
        key: 'frame',
        minWidthPx: 100,
        header: m(GridHeaderCell, {}, 'App Frame'),
      },
    ];

    return m(
      DetailsShell,
      {title: this.getTitle()},
      m(Grid, {
        columns,
        rowData: this.rows.map((row) => [
          m(
            GridCell,
            {},
            m(Checkbox, {
              checked: this.visibleRowIds.has(row.uiRowId),
              onchange: () => this.toggleVisibility(row.uiRowId),
            }),
          ),
          m(
            GridCell,
            {},
            m(Checkbox, {
              checked: this.pinnedRowIds.has(row.uiRowId),
              onchange: () => this.togglePinning(row),
            }),
          ),
          m(GridCell, {}, row.channel),
          m(
            GridCell,
            {},
            row.totalLatency !== null
              ? m(DurationWidget, {
                  dur: row.totalLatency,
                  trace: this.trace,
                })
              : '-',
          ),
          this.renderCell(row.durReader, row.navReader),
          this.renderCell(row.deltaDispatch, row.navDispatch),
          this.renderCell(row.deltaReceive, row.navReceive),
          this.renderCell(row.deltaConsume, row.navConsume),
          this.renderCell(row.deltaFrame, row.navFrame),
        ]),
        emptyState: m(EmptyState, {
          title: 'No input event selected',
          description: 'Select an input event to see latency breakdown.',
          icon: Icons.Android,
        }),
      }),
    );
  }

  private renderCell(dur: duration | null, nav?: NavTarget) {
    return m(
      GridCell,
      {},
      dur !== null
        ? m(DurationWidget, {dur, trace: this.trace})
        : m('span', '-'),
      nav &&
        m(Anchor, {
          icon: Icons.GoTo,
          onclick: () => this.goTo(nav),
          title: 'Go to event slice',
        }),
    );
  }
}
