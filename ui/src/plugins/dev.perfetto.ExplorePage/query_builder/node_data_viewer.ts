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

import {TextParagraph} from '../../../widgets/text_paragraph';
import {QueryTable} from '../../../components/query_table/query_table';
import {runQueryForQueryTable} from '../../../components/query_table/queries';
import {AsyncLimiter} from '../../../base/async_limiter';
import {QueryResponse} from '../../../components/query_table/queries';
import {Trace} from '../../../public/trace';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {Query, queryToRun} from '../query_node';

export interface NodeDataViewerAttrs {
  readonly query?: Query | Error;
  readonly executeQuery: boolean;
  readonly trace: Trace;
  readonly onQueryExecuted: (result: {
    columns: string[];
    queryError?: Error;
    responseError?: Error;
    dataError?: Error;
  }) => void;
  readonly onPositionChange: (pos: 'left' | 'right' | 'bottom') => void;
}

export class NodeDataViewer implements m.ClassComponent<NodeDataViewerAttrs> {
  private readonly asyncLimiter = new AsyncLimiter();
  private resp?: QueryResponse;

  view({attrs}: m.CVnode<NodeDataViewerAttrs>) {
    const isQueryInvalid = (): Error | undefined => {
      if (attrs.query instanceof Error) {
        return attrs.query;
      }
      if (!this.resp) {
        return undefined;
      }

      if (this.resp.error) {
        return new Error(this.resp.error);
      }
      return undefined;
    };

    const isResponseInvalid = (): Error | undefined => {
      // Those are the checks that should be handled by isQueryInvalid().
      if (!this.resp || this.resp.error) {
        return undefined;
      }

      if (
        this.resp.statementCount > 0 &&
        this.resp.statementWithOutputCount === 0 &&
        this.resp.columns.length === 0
      ) {
        return new Error('The last statement must produce an output.');
      }

      if (this.resp.statementWithOutputCount > 1) {
        return new Error('Only the last statement can produce an output.');
      }

      if (this.resp.statementCount > 1) {
        // Statements are broken by semicolon. We trim and remove empty statements
        // that can result from the query ending with a semicolon.
        // TODO(mayzner): This logic has to be implemented in Trace Processor.
        const statements = this.resp.query
          .split(';')
          .map((x) => x.trim())
          .filter((x) => x.length > 0);
        const allButLast = statements.slice(0, statements.length - 1);
        const moduleIncludeRegex =
          /^\s*INCLUDE\s+PERFETTO\s+MODULE\s+[\w._]+\s*$/i;
        for (const stmt of allButLast) {
          if (!moduleIncludeRegex.test(stmt)) {
            return new Error(
              `Only 'INCLUDE PERFETTO MODULE ...;' statements are ` +
                `allowed before the final statement. Error on: "${stmt}"`,
            );
          }
        }
      }

      return undefined;
    };

    const runQuery = () => {
      this.asyncLimiter.schedule(async () => {
        if (
          attrs.query === undefined ||
          attrs.query instanceof Error ||
          !attrs.executeQuery
        ) {
          return;
        }

        this.resp = await runQueryForQueryTable(
          queryToRun(attrs.query),
          attrs.trace.engine,
        );

        let dataError: Error | undefined = undefined;
        if (this.resp.totalRowCount === 0) {
          dataError = new Error('Query returned no rows');
        }

        attrs.onQueryExecuted({
          columns: this.resp.columns,
          queryError: isQueryInvalid(),
          responseError: isResponseInvalid(),
          dataError,
        });
      });
    };

    runQuery();
    const queryError = isQueryInvalid();
    const responseError = isResponseInvalid();
    const error = queryError ?? responseError;

    let statusText: string | undefined = undefined;
    if (attrs.query === undefined) {
      statusText = 'No data to display';
    } else if (this.resp === undefined) {
      statusText = 'Typing...';
    }

    const message = error ? `Error: ${error.message}` : statusText;

    return [
      m(
        '.pf-node-data-viewer',
        m(
          '.pf-node-data-viewer__title-row',
          m('.title', 'Query data'),
          m('span.spacer'), // Push menu to the right
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                icon: Icons.ContextMenuAlt,
              }),
            },
            [
              m(MenuItem, {
                label: 'Left',
                onclick: () => {
                  attrs.onPositionChange('left');
                },
              }),
              m(MenuItem, {
                label: 'Right',
                onclick: () => {
                  attrs.onPositionChange('right');
                },
              }),
              m(MenuItem, {
                label: 'Bottom',
                onclick: () => {
                  attrs.onPositionChange('bottom');
                },
              }),
            ],
          ),
        ),
        message && !responseError
          ? m(TextParagraph, {text: message})
          : m(
              'article',
              {
                style: {
                  display: 'flex',
                  flexDirection: 'column',
                  flexGrow: 1,
                },
              },
              [
                m(QueryTable, {
                  trace: attrs.trace,
                  query:
                    attrs.query instanceof Error ? '' : queryToRun(attrs.query),
                  resp: this.resp,
                  fillParent: true,
                }),
              ],
            ),
      ),
    ];
  }
}
