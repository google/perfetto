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
import {ArgsDict} from '../sql_utils/args';
import {Trace} from '../../public/trace';
import {renderArguments} from './args';
import {extensions} from '../extensions';
import {sqliteString} from '../../base/string_utils';
import {SLICE_TABLE} from '../widgets/sql/table_definitions';

// Renders slice arguments (key/value pairs) as a subtree.
export function renderSliceArguments(trace: Trace, args: ArgsDict): m.Children {
  return renderArguments(trace, args, (key, value) => {
    const displayValue = value === null ? 'NULL' : String(value);
    return [
      m(MenuItem, {
        label: 'Find slices with same arg value',
        icon: 'search',
        onclick: () => {
          extensions.addLegacySqlTableTab(trace, {
            table: SLICE_TABLE,
            filters: [
              {
                op: (cols) => `${cols[0]} = ${sqliteString(displayValue)}`,
                columns: [
                  {
                    column: 'display_value',
                    source: {
                      table: 'args',
                      joinOn: {
                        arg_set_id: 'arg_set_id',
                        key: sqliteString(key),
                      },
                    },
                  },
                ],
              },
            ],
          });
        },
      }),
      m(MenuItem, {
        label: 'Visualize argument values',
        icon: 'query_stats',
        onclick: () => {
          extensions.addVisualizedArgTracks(trace, key);
        },
      }),
    ];
  });
}
