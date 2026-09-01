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
import {assertExists} from '../../base/assert';
import type {Trace} from '../../public/trace';
import type {QueryLog} from '../../trace_processor/engine';
import {Button} from '../../widgets/button';
import {Chip} from '../../widgets/chip';
import {Intent} from '../../widgets/common';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {DetailsShell} from '../../widgets/details_shell';

function formatMillis(millis: number) {
  return millis.toFixed(1);
}

interface QueryLogEntryAttrs {
  readonly queryLog: QueryLog;
}

class QueryLogRow implements m.ClassComponent<QueryLogEntryAttrs> {
  private isOpen = false;

  view({attrs}: m.Vnode<QueryLogEntryAttrs>) {
    const {queryLog: ql} = attrs;
    assertExists(ql.query);
    const fullQuery = ql.query.trim();
    const isOpen = this.isOpen;
    const toggle = () => {
      this.isOpen = !this.isOpen;
    };
    const statusChip =
      ql.success === undefined
        ? m(Chip, {label: 'Running', intent: Intent.Warning, compact: true})
        : ql.success
          ? m(Chip, {label: 'Completed', intent: Intent.Success, compact: true})
          : m(Chip, {label: 'Failed', intent: Intent.Danger, compact: true});
    return m(
      'tr.pf-query-log-row',
      {class: isOpen ? 'pf-expanded' : ''},
      m('td.pf-query-log-num', formatMillis(ql.startTime)),
      m(
        'td.pf-query-log-num',
        ql.elapsedTimeMs === undefined ? '...' : formatMillis(ql.elapsedTimeMs),
      ),
      m('td', ql.tag),
      m('td', statusChip),
      m(
        'td.pf-query-log-toggle',
        m(Button, {
          icon: isOpen ? 'expand_more' : 'chevron_right',
          compact: true,
          onclick: toggle,
        }),
      ),
      m(
        'td.pf-query-log-query',
        {
          title: isOpen ? undefined : fullQuery,
          onclick: toggle,
        },
        isOpen
          ? m(
              '.pf-query-log-expanded-body',
              m(
                '.pf-query-log-expanded-toolbar',
                {onclick: (e: Event) => e.stopPropagation()},
                m(CopyToClipboardButton, {
                  textToCopy: fullQuery,
                  label: 'Copy',
                  compact: true,
                }),
              ),
              m('span.pf-query-log-full', fullQuery),
            )
          : m('span.pf-query-log-snippet', fullQuery.replace(/\s+/g, ' ')),
      ),
    );
  }
}

export interface QueryTabAttrs {
  readonly trace: Trace;
}

const PAGE_SIZE = 100;

export class QueryTab implements m.ClassComponent<QueryTabAttrs> {
  private visibleCount = PAGE_SIZE;

  view({attrs}: m.Vnode<QueryTabAttrs>) {
    const {trace} = attrs;
    // Show the logs in reverse order
    const queryLog = Array.from(trace.engine.queryLog).reverse();
    const visible = queryLog.slice(0, this.visibleCount);
    const remaining = queryLog.length - visible.length;
    return m(
      DetailsShell,
      {
        title: 'Query Log',
        description: `${queryLog.length} ${queryLog.length === 1 ? 'entry' : 'entries'}`,
        buttons: m(Button, {
          label: 'Clear',
          icon: 'delete',
          disabled: queryLog.length === 0,
          onclick: () => trace.engine.clearQueryLog(),
        }),
      },
      m(
        '.pf-query-log',
        m(
          'table.pf-query-log-table',
          m(
            'thead',
            m(
              'tr',
              m('th.pf-query-log-num', 'Start (ms)'),
              m('th.pf-query-log-num', 'Duration (ms)'),
              m('th', 'Tag'),
              m('th', 'Status'),
              m('th'),
              m('th', 'Query'),
            ),
          ),
          m(
            'tbody',
            visible.map((ql) => m(QueryLogRow, {queryLog: ql})),
          ),
        ),
        remaining > 0 &&
          m(
            '.pf-query-log-show-more',
            m(Button, {
              label: `Show ${Math.min(PAGE_SIZE, remaining)} more (${remaining} remaining)`,
              onclick: () => {
                this.visibleCount += PAGE_SIZE;
              },
            }),
          ),
      ),
    );
  }
}
