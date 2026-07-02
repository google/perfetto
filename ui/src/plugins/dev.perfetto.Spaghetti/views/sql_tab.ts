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
import {Button, ButtonVariant} from '../../../widgets/button';
import './sql_tab.scss';

export interface SqlTabAttrs {
  readonly displaySql: string | undefined;
  readonly sqlText: string;
}

function renderPreBlock(text: string, hasContent: boolean): m.Children {
  return m('pre', {className: hasContent ? undefined : 'pf-empty'}, text);
}

export class SqlTab implements m.ClassComponent<SqlTabAttrs> {
  view({attrs}: m.Vnode<SqlTabAttrs>) {
    const {displaySql, sqlText} = attrs;
    return m('.pf-spag-sql-tab', [
      displaySql
        ? m(
            '.pf-spag-sql-tab-toolbar',
            m(Button, {
              variant: ButtonVariant.Filled,
              icon: 'content_copy',
              label: 'Copy',
              onclick: () => {
                navigator.clipboard.writeText(displaySql);
              },
            }),
          )
        : null,
      renderPreBlock(sqlText, !!displaySql),
    ]);
  }
}
