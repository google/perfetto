// Copyright (C) 2025 The Android Open Source Project
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
import {assertExists} from '../../base/logging';
import {Icons} from '../../base/semantic_icons';
import {PivotTable} from '../../components/widgets/sql/pivot_table/pivot_table';
import {PivotTableState} from '../../components/widgets/sql/pivot_table/pivot_table_state';
import {
  AreaSelection,
  areaSelectionsEqual,
  AreaSelectionTab,
} from '../../public/selection';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Button} from '../../widgets/button';
import {Trace} from '../../public/trace';
import {extensions} from '../../components/extensions';
import {SLICE_TABLE} from '../../components/widgets/sql/table_definitions';
import {resolveTableDefinition} from '../../components/widgets/sql/table/columns';

export class PivotTableTab implements AreaSelectionTab {
  readonly id = 'pivot_table';
  readonly name = 'Pivot Table';

  private state?: PivotTableState;
  private previousSelection?: AreaSelection;
  private trackIds: number[] = [];

  constructor(private readonly trace: Trace) {}

  render(selection: AreaSelection) {
    if (
      this.previousSelection === undefined ||
      !areaSelectionsEqual(this.previousSelection, selection)
    ) {
      this.previousSelection = selection;
      this.trackIds = selection.tracks
        .filter((track) => track.tags?.kinds?.includes(SLICE_TRACK_KIND))
        .flatMap((track) => track.tags?.trackIds ?? []);
      this.getOrCreateState().filters.setFilters([
        {
          op: (cols) => `${cols[0]} + ${cols[1]} > ${selection.start}`,
          columns: ['ts', 'dur'],
        },
        {op: (cols) => `${cols[0]} < ${selection.end}`, columns: ['ts']},
        {
          op: (cols) => `${cols[0]} in (${this.trackIds.join(', ')})`,
          columns: ['track_id'],
        },
      ]);
    }
    if (this.trackIds.length === 0) return undefined;
    const state = this.getOrCreateState();

    return {
      isLoading: state?.getData() === undefined,
      content: m(PivotTable, {
        state,
        getSelectableColumns: () => state.table.columns,
        extraRowButton: (node) =>
          m(Button, {
            icon: Icons.GoTo,
            onclick: () => {
              extensions.addLegacySqlTableTab(this.trace, {
                table: SLICE_TABLE,
                filters: [
                  ...(state?.filters.get() ?? []),
                  ...node.getFilters(),
                ],
              });
            },
          }),
      }),
    };
  }

  private getOrCreateState(): PivotTableState {
    if (this.state !== undefined) return this.state;
    const sliceTable = resolveTableDefinition(this.trace, SLICE_TABLE);
    const name = assertExists(
      sliceTable.columns.find((c) => c.column === 'name'),
    );
    const dur = assertExists(
      sliceTable.columns.find((c) => c.column === 'dur'),
    );
    this.state = new PivotTableState({
      trace: this.trace,
      table: sliceTable,
      pivots: [name],
      aggregations: [
        {
          column: dur,
          op: 'sum',
        },
      ],
    });
    return this.state;
  }
}
