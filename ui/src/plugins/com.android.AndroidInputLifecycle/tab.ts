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

import {
  type GridColumn,
  GridHeaderCell,
  Grid,
  GridCell,
} from '../../widgets/grid';
import type {TrackPinningManager} from '../../components/related_events/utils';
import {Icons} from '../../base/semantic_icons';
import type {duration} from '../../base/time';
import {DurationWidget} from '../../components/widgets/duration';
import type {Trace} from '../../public/trace';
import {Anchor} from '../../widgets/anchor';
import {Checkbox} from '../../widgets/checkbox';

import {
  AndroidInputEventSource,
  type InputChainRow,
} from './android_input_event_source';
import type {InputLifecycleExtension, NavTarget} from './extensions/interface';

export interface AndroidInputLifecycleTabAttrs {
  readonly trace: Trace;
  readonly rows: ReadonlyArray<InputChainRow>;
  readonly visibleRowIds: ReadonlySet<string>;
  readonly pinningManager: TrackPinningManager;
  readonly onToggleVisibility: (rowId: string) => void;
  readonly onToggleAllVisibility: () => void;
  readonly activeExtensions: ReadonlyArray<InputLifecycleExtension>;
}

export class AndroidInputLifecycleTab implements m.ClassComponent<AndroidInputLifecycleTabAttrs> {
  view({attrs}: m.Vnode<AndroidInputLifecycleTabAttrs>): m.Children {
    const {rows, visibleRowIds, trace, pinningManager} = attrs;
    const allVisible =
      rows.length > 0 && rows.every((r) => visibleRowIds.has(r.uiRowId));

    const specs = AndroidInputEventSource.getStageSpecs(attrs.activeExtensions);

    const columns: GridColumn[] = [
      {
        key: 'show',
        widthPx: 40,
        header: m(
          GridHeaderCell,
          {},
          m(Checkbox, {
            checked: allVisible,
            onchange: () => attrs.onToggleAllVisibility(),
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
      ...specs.map((spec) => ({
        key: spec.key,
        minWidthPx: 100,
        header: m(GridHeaderCell, {}, spec.headerName),
      })),
    ];

    return m(Grid, {
      columns,
      rowData: rows.map((row) => {
        const fixedCells = [
          m(
            GridCell,
            {},
            m(Checkbox, {
              checked: visibleRowIds.has(row.uiRowId),
              onchange: () => attrs.onToggleVisibility(row.uiRowId),
            }),
          ),
          m(
            GridCell,
            {},
            m(Checkbox, {
              checked: isRowPinned(row, pinningManager),
              onchange: () => togglePinning(row, pinningManager),
            }),
          ),
          m(GridCell, {}, row.channel),
          m(
            GridCell,
            {},
            row.totalLatency !== null
              ? m(DurationWidget, {dur: row.totalLatency, trace})
              : '-',
          ),
        ];

        const stageCells = specs.map((spec) => {
          const cellData = row.stagesData.get(spec.key);
          return renderCell(cellData?.dur ?? null, cellData?.nav, trace);
        });

        return [...fixedCells, ...stageCells];
      }),
    });
  }
}

function isRowPinned(
  row: InputChainRow,
  pinningManager: TrackPinningManager,
): boolean {
  return (
    row.allTrackUris.length > 0 &&
    row.allTrackUris.every((uri) => pinningManager.isTrackPinned(uri))
  );
}

function togglePinning(
  row: InputChainRow,
  pinningManager: TrackPinningManager,
) {
  if (isRowPinned(row, pinningManager)) {
    pinningManager.unpinTracks(row.allTrackUris);
  } else {
    pinningManager.pinTracks(row.allTrackUris);
  }
}

function renderCell(
  dur: duration | null,
  nav: NavTarget | undefined,
  trace: Trace,
) {
  return m(
    GridCell,
    {},
    dur !== null ? m(DurationWidget, {dur, trace}) : m('span', '-'),
    nav !== undefined &&
      m(Anchor, {
        icon: Icons.GoTo,
        onclick: () => {
          trace.selection.selectTrackEvent(nav.trackUri, nav.id, {
            scrollToSelection: true,
            switchToCurrentSelectionTab: false,
          });
        },
        title: 'Go to event slice',
      }),
  );
}
