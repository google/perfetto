// Copyright (C) 2023 The Android Open Source Project
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

import {EngineProxy} from '../../common/engine';
import {STR} from '../../common/query_result';
import {raf} from '../../core/raf_scheduler';
import {FilterableSelect} from '../../widgets/select';
import {Spinner} from '../../widgets/spinner';
import {
  constraintsToQueryPrefix,
  constraintsToQuerySuffix,
  SQLConstraints,
} from '../sql_utils';

import {argColumn} from './column';
import {ArgSetIdColumn} from './table_description';

const MAX_ARGS_TO_DISPLAY = 15;

interface ArgumentSelectorAttrs {
  engine: EngineProxy;
  argSetId: ArgSetIdColumn;
  tableName: string;
  constraints: SQLConstraints;
  // List of aliases for existing columns by the table.
  alreadySelectedColumns: Set<string>;
  onArgumentSelected: (argument: string) => void;
}

// A widget which allows the user to select a new argument to display.
// Dinamically queries Trace Processor to find the relevant set of arg_set_ids
// and which args are present in these arg sets.
export class ArgumentSelector implements
    m.ClassComponent<ArgumentSelectorAttrs> {
  argList?: string[];

  constructor({attrs}: m.Vnode<ArgumentSelectorAttrs>) {
    this.load(attrs);
  }

  private async load(attrs: ArgumentSelectorAttrs) {
    const queryResult = await attrs.engine.query(`
      -- Encapsulate the query in a CTE to avoid clashes between filters
      -- and columns of the 'args' table.
      WITH arg_sets AS (
        ${constraintsToQueryPrefix(attrs.constraints)}
        SELECT DISTINCT ${attrs.tableName}.${attrs.argSetId.name} as arg_set_id
        FROM ${attrs.tableName}
        ${constraintsToQuerySuffix(attrs.constraints)}
      )
      SELECT
        DISTINCT args.key as key
      FROM arg_sets
      JOIN args USING (arg_set_id)
    `);
    this.argList = [];
    const it = queryResult.iter({key: STR});
    for (; it.valid(); it.next()) {
      const arg = argColumn(attrs.argSetId, it.key);
      if (attrs.alreadySelectedColumns.has(arg.alias)) continue;
      this.argList.push(it.key);
    }
    raf.scheduleFullRedraw();
  }

  view({attrs}: m.Vnode<ArgumentSelectorAttrs>) {
    if (this.argList === undefined) return m(Spinner);
    return m(FilterableSelect, {
      values: this.argList,
      onSelected: (value: string) => attrs.onArgumentSelected(value),
      maxDisplayedItems: MAX_ARGS_TO_DISPLAY,
      autofocusInput: true,
    });
  }
}
