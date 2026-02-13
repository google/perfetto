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
import z from 'zod';
import {GridColumn, GridHeaderCell, Grid, GridCell} from '../../widgets/grid';
import {Spinner} from '../../widgets/spinner';
import {AndroidInputEventSource} from './android_input_event_source';
import {
  getTrackUriForTrackId,
  TrackPinningManager,
  NavTarget,
  durationSchema,
  NavTargetSchema,
} from '../dev.perfetto.RelatedEvents';
import {Icons} from '../../base/semantic_icons';
import {duration} from '../../base/time';
import {DurationWidget} from '../../components/widgets/duration';
import {Tab} from '../../public/tab';
import {Trace} from '../../public/trace';
import {Anchor} from '../../widgets/anchor';
import {Checkbox} from '../../widgets/checkbox';
import {DetailsShell} from '../../widgets/details_shell';
import {EmptyState} from '../../widgets/empty_state';

// --- Zod Schemas ---

const StageSchema = z
  .object({
    delta: durationSchema.nullable(),
    dur: durationSchema,
    nav: NavTargetSchema,
  })
  .nullable();

const InputLifecycleArgsSchema = z.object({
  channel: z.string(),
  totalLatency: durationSchema.nullable(),
  reader: z
    .object({
      dur: durationSchema,
      nav: NavTargetSchema,
    })
    .nullable(),
  dispatcher: StageSchema,
  receiver: StageSchema,
  consumer: StageSchema,
  frame: StageSchema,
  allTrackIds: z.array(z.number()),
});

// --- Interfaces ---

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
  allTrackUris: string[];
}

export class AndroidInputLifecycleTab implements Tab {
  private rows: InputChainRow[] = [];
  private visibleRowIds = new Set<string>();
  private currentSelectionId?: number;
  private isLoading = false;

  constructor(
    private trace: Trace,
    private source: AndroidInputEventSource,
    private pinningManager: TrackPinningManager,
  ) {}

  onHide() {
    this.rows = [];
    this.visibleRowIds.clear();
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
    this.rows = [];
    this.visibleRowIds.clear();

    try {
      const data = await this.source.getRelatedEventData(clickedEventId);
      let index = 0;
      for (const event of data.events) {
        if (event.type === 'InputLifecycle') {
          const parsedArgs = InputLifecycleArgsSchema.safeParse(
            event.customArgs,
          );
          if (parsedArgs.success) {
            const args = parsedArgs.data;
            const uniqueId = `row-${index++}`;
            const allTrackIds = args.allTrackIds;
            const allTrackUris = allTrackIds.map((id: number) =>
              getTrackUriForTrackId(this.trace, id),
            );
            this.rows.push({
              uiRowId: uniqueId,
              channel: args.channel,
              totalLatency: args.totalLatency,
              durReader: args.reader?.dur ?? null,
              deltaDispatch: args.dispatcher?.delta ?? null,
              deltaReceive: args.receiver?.delta ?? null,
              deltaConsume: args.consumer?.delta ?? null,
              deltaFrame: args.frame?.delta ?? null,
              navReader: args.reader?.nav,
              navDispatch: args.dispatcher?.nav,
              navReceive: args.receiver?.nav,
              navConsume: args.consumer?.nav,
              navFrame: args.frame?.nav,
              allTrackIds,
              allTrackUris,
            });
            this.visibleRowIds.add(uniqueId);
          } else {
            console.error(
              'Invalid customArgs for InputLifecycle event',
              parsedArgs.error,
            );
          }
        }
      }
      this.pinningManager.applyPinning(this.trace);
    } finally {
      this.isLoading = false;
    }
  }

  private getRowTrackUris(row: InputChainRow): string[] {
    return row.allTrackUris;
  }

  private isRowPinned(row: InputChainRow): boolean {
    const trackUris = this.getRowTrackUris(row);
    return (
      trackUris.length > 0 &&
      trackUris.every((uri) => this.pinningManager.isTrackPinned(uri))
    );
  }

  private toggleVisibility(rowId: string) {
    if (this.visibleRowIds.has(rowId)) this.visibleRowIds.delete(rowId);
    else this.visibleRowIds.add(rowId);
  }

  private toggleAllVisibility() {
    const allVisible = this.rows.every((r) =>
      this.visibleRowIds.has(r.uiRowId),
    );
    if (allVisible) this.visibleRowIds.clear();
    else this.rows.forEach((r) => this.visibleRowIds.add(r.uiRowId));
  }

  private togglePinning(row: InputChainRow) {
    const trackUris = this.getRowTrackUris(row);
    const currentlyPinned = this.isRowPinned(row);

    if (currentlyPinned) {
      this.pinningManager.unpinTracks(trackUris);
    } else {
      this.pinningManager.pinTracks(trackUris);
    }
    this.pinningManager.applyPinning(this.trace);
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
              checked: this.isRowPinned(row),
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
      nav !== undefined &&
        m(Anchor, {
          icon: Icons.GoTo,
          onclick: () => this.goTo(nav),
          title: 'Go to event slice',
        }),
    );
  }
}
