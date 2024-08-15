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

import m from 'mithril';

import {raf} from '../../../../core/raf_scheduler';
import {FilterableSelect} from '../../../../widgets/select';
import {Spinner} from '../../../../widgets/spinner';

import {
  TableColumn,
  tableColumnId,
  TableColumnSet,
  TableManager,
} from './column';

const MAX_ARGS_TO_DISPLAY = 15;

interface ArgumentSelectorAttrs {
  tableManager: TableManager;
  columnSet: TableColumnSet;
  alreadySelectedColumnIds: Set<string>;
  onArgumentSelected: (column: TableColumn) => void;
}

// This class is responsible for rendering a menu which allows user to select which column out of ColumnSet to add.
export class ArgumentSelector
  implements m.ClassComponent<ArgumentSelectorAttrs>
{
  columns?: {[key: string]: TableColumn};

  constructor({attrs}: m.Vnode<ArgumentSelectorAttrs>) {
    this.load(attrs);
  }

  private async load(attrs: ArgumentSelectorAttrs) {
    const potentialColumns = await attrs.columnSet.discover(attrs.tableManager);
    this.columns = Object.fromEntries(
      potentialColumns
        .filter(
          ({column}) =>
            !attrs.alreadySelectedColumnIds.has(tableColumnId(column)),
        )
        .map(({key, column}) => [key, column]),
    );
    raf.scheduleFullRedraw();
  }

  view({attrs}: m.Vnode<ArgumentSelectorAttrs>) {
    const columns = this.columns;
    if (columns === undefined) return m(Spinner);
    return m(FilterableSelect, {
      values: Object.keys(columns),
      onSelected: (value: string) => attrs.onArgumentSelected(columns[value]),
      maxDisplayedItems: MAX_ARGS_TO_DISPLAY,
      autofocusInput: true,
    });
  }
}
