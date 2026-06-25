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

export interface SqlTabAttrs {
  readonly displaySql: string | undefined;
  readonly sqlText: string;
}

function renderPreBlock(text: string, hasContent: boolean): m.Children {
  return m(
    'pre',
    {
      style: {
        margin: '0',
        padding: '8px',
        overflow: 'auto',
        flex: '1',
        fontFamily: 'monospace',
        fontSize: '12px',
        whiteSpace: 'pre-wrap',
        wordBreak: 'break-word',
        opacity: hasContent ? '1' : '0.5',
      },
    },
    text,
  );
}

export function renderSqlTab(attrs: SqlTabAttrs): m.Children {
  const {displaySql, sqlText} = attrs;
  return m(
    '',
    {
      style: {
        display: 'flex',
        flexDirection: 'column',
        flex: '1',
        overflow: 'hidden',
      },
    },
    [
      displaySql
        ? m(
            '',
            {
              style: {
                display: 'flex',
                justifyContent: 'flex-end',
                padding: '4px 8px 0',
                gap: '4px',
              },
            },
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
    ],
  );
}
