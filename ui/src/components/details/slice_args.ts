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
import {MenuItem} from '../../widgets/menu';
import {ArgsDict, ArgValue} from '../sql_utils/args';
import {Trace} from '../../public/trace';
import {renderArguments} from './args';
import {extensions} from '../extensions';
import {SqlValue} from '../../trace_processor/query_result';
import {DataGridFilter} from '../widgets/datagrid/common';

// Convert ArgValue (which includes boolean) to SqlValue (which doesn't)
function argValueToSqlValue(value: ArgValue): SqlValue {
  if (typeof value === 'boolean') {
    // Convert boolean to number for SQL compatibility
    return value ? 1 : 0;
  }
  return value;
}

export interface SliceArgsOptions {
  // Optional callback to open a table explorer with filters. If not provided,
  // "Find slices with same arg value" menu item will be omitted.
  readonly openTableExplorer?: (
    tableName: string,
    options?: {filters?: DataGridFilter[]},
  ) => void;
}

// Renders slice arguments (key/value pairs) as a subtree.
export function renderSliceArguments(
  trace: Trace,
  args: ArgsDict,
  options?: SliceArgsOptions,
): m.Children {
  return renderArguments(trace, args, (key, value) => {
    const menuItems: m.Children[] = [];

    // Only show "Find slices" if openTableExplorer callback is provided
    if (options?.openTableExplorer) {
      menuItems.push(
        m(MenuItem, {
          label: 'Find slices with same arg value',
          icon: 'search',
          onclick: () => {
            // Use parameterized column filter: args.{key} = value
            options.openTableExplorer!('slice', {
              filters: [
                {
                  column: `args.${key}`,
                  op: '=',
                  value: argValueToSqlValue(value),
                },
              ],
            });
          },
        }),
      );
    }

    menuItems.push(
      m(MenuItem, {
        label: 'Visualize argument values',
        icon: 'query_stats',
        onclick: () => {
          extensions.addVisualizedArgTracks(trace, key);
        },
      }),
    );

    return menuItems;
  });
}
