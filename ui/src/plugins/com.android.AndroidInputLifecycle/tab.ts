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

import {GridColumn, GridHeaderCell, Grid, GridCell} from '../../widgets/grid';
import {TrackPinningManager} from '../../components/related_events/utils';
import {Icons} from '../../base/semantic_icons';
import {duration} from '../../base/time';
import {DurationWidget} from '../../components/widgets/duration';
import {Trace} from '../../public/trace';
import {Anchor} from '../../widgets/anchor';
import {Checkbox} from '../../widgets/checkbox';
import {EmptyState} from '../../widgets/empty_state';
import {DetailsShell} from '../../widgets/details_shell';
import {Spinner} from '../../widgets/spinner';

import {InputChainRow, NavTarget} from './android_input_event_source';

export interface AndroidInputLifecycleTabAttrs {
  trace: Trace;
  rows: InputChainRow[];
  visibleRowIds: Set<string>;
  loading: boolean;
  pinningManager: TrackPinningManager;
  onToggleVisibility: (rowId: string) => void;
  onToggleAllVisibility: () => void;
}

export class AndroidInputLifecycleTab
  implements m.ClassComponent<AndroidInputLifecycleTabAttrs>
{
  view({attrs}: m.Vnode<AndroidInputLifecycleTabAttrs>): m.Children {
    if (attrs.loading) {
      return m(
        DetailsShell,
        {title: 'Android Input Lifecycle'},
        m(
          'div',
          {
            style: {
              display: 'flex',
              justifyContent: 'center',
              padding: '20px',
            },
          },
          m(Spinner, {}),
        ),
      );
    }

    return m(
      DetailsShell,
      {title: 'Android Input Lifecycle'},
      this.renderGrid(attrs),
    );
  }

  private renderGrid(attrs: AndroidInputLifecycleTabAttrs): m.Children {
    const {rows, visibleRowIds, trace, pinningManager} = attrs;
    const allVisible =
      rows.length > 0 && rows.every((r) => visibleRowIds.has(r.uiRowId));

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
      {key: 'chan', header: m(GridHeaderCell, {}, 'Channel')},
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

    return m(Grid, {
      columns,
      rowData: rows.map((row) => [
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
            onchange: () => togglePinning(row, trace, pinningManager),
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
        renderCell(row.durReader, row.navReader, trace),
        renderCell(row.deltaDispatch, row.navDispatch, trace),
        renderCell(row.deltaReceive, row.navReceive, trace),
        renderCell(row.deltaConsume, row.navConsume, trace),
        renderCell(row.deltaFrame, row.navFrame, trace),
      ]),
      emptyState: m(EmptyState, {
        title: 'No input event selected',
        description: 'Select an input event to see latency breakdown.',
        icon: Icons.Android,
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
  trace: Trace,
  pinningManager: TrackPinningManager,
) {
  if (isRowPinned(row, pinningManager)) {
    pinningManager.unpinTracks(row.allTrackUris);
  } else {
    pinningManager.pinTracks(row.allTrackUris);
  }
  pinningManager.applyPinning(trace);
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
