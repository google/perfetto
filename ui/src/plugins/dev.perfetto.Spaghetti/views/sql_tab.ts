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
import {CodeSnippet} from '../../../widgets/code_snippet';
import {EmptyState} from '../../../widgets/empty_state';
import './sql_tab.scss';

export interface SqlTabAttrs {
  readonly displaySql: string | undefined;
  readonly activeNodeId: string | undefined;
  readonly language: string;
}

export class SqlTab implements m.ClassComponent<SqlTabAttrs> {
  view({attrs}: m.Vnode<SqlTabAttrs>) {
    const {displaySql, activeNodeId, language} = attrs;
    if (!displaySql) {
      if (activeNodeId) {
        return m(
          EmptyState,
          {
            icon: 'warning',
            fillHeight: true,
            title: 'Incomplete query',
          },
          'Fill in all required fields to preview SQL.',
        );
      }
      return m(
        EmptyState,
        {
          fillHeight: true,
          title: 'No node selected',
        },
        'Click on a node to preview its SQL.',
      );
    }
    return m(
      '.pf-spag-sql-tab',
      m(CodeSnippet, {
        language,
        text: displaySql,
      }),
    );
  }
}
