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
import {classNames} from '../../../base/classnames';
import {ButtonVariant} from '../../../widgets/button';
import {Callout} from '../../../widgets/callout';
import {CopyToClipboardButton} from '../../../widgets/copy_to_clipboard_button';
import {EmptyState} from '../../../widgets/empty_state';
import {Card} from '../components/card';
import type {ChunkQueriesResult} from '../chunk_queries';
import type {StepRunOutcome} from '../run_chunks';
import type {ChunkPlan} from '../plan_chunks';
import type {SerializeResult} from '../serialize_common';
import './materialization_tab.scss';

export interface MaterializationTabAttrs {
  readonly chunkPlan: ChunkPlan | undefined;
  readonly chunkQueries: ChunkQueriesResult | undefined;
  /** Whether the output chunk is built into a table. */
  readonly materializeOutput: boolean;
  /** The query that reads the output, in the current dialect. */
  readonly output: SerializeResult | undefined;
  /** Label for the current dialect. */
  readonly language: string;
  readonly lastRunOutcomes: ReadonlyMap<string, StepRunOutcome>;
  readonly activeNodeId: string | undefined;
}

// A query with its includes at the top.
function withIncludes(includes: readonly string[], sql: string): string {
  if (includes.length === 0) return sql;
  const headers = includes.map((inc) => `INCLUDE PERFETTO MODULE ${inc};`);
  return `${headers.join('\n')}\n\n${sql}`;
}

export class MaterializationTab implements m.ClassComponent<MaterializationTabAttrs> {
  private highlightedHash: string | undefined;
  private rootEl: HTMLElement | undefined;

  oncreate(vnode: m.VnodeDOM<MaterializationTabAttrs>) {
    this.rootEl = vnode.dom as HTMLElement;
  }

  onupdate(vnode: m.VnodeDOM<MaterializationTabAttrs>) {
    this.rootEl = vnode.dom as HTMLElement;
  }

  onremove() {
    this.rootEl = undefined;
  }

  private scrollToCard(hash: string) {
    this.highlightedHash = hash;
    const el = this.rootEl?.querySelector<HTMLElement>(
      `[data-card-hash="${hash}"]`,
    );
    el?.scrollIntoView({behavior: 'smooth', block: 'nearest'});
  }

  private renderDepsRow(label: string, deps: readonly string[]): m.Children {
    if (deps.length === 0) return undefined;
    return [
      m('span.pf-spag-mat-deps-label', label),
      ...deps.map((dep) =>
        m(
          'button.pf-spag-mat-dep-chip',
          {
            type: 'button',
            title: `Jump to ${dep}`,
            onmouseenter: () => {
              this.highlightedHash = dep;
            },
            onmouseleave: () => {
              if (this.highlightedHash === dep) {
                this.highlightedHash = undefined;
              }
            },
            onclick: () => this.scrollToCard(dep),
          },
          `← ${dep}`,
        ),
      ),
    ];
  }

  view({attrs}: m.Vnode<MaterializationTabAttrs>) {
    const {
      chunkPlan,
      chunkQueries,
      materializeOutput,
      output,
      language,
      lastRunOutcomes,
      activeNodeId,
    } = attrs;

    if (chunkPlan === undefined || chunkQueries === undefined) {
      if (activeNodeId) {
        return m(
          EmptyState,
          {
            icon: 'warning',
            fillHeight: true,
            title: 'Incomplete query',
          },
          'Fill in all required fields to inspect the materializer plan.',
        );
      }
      return m(
        EmptyState,
        {
          fillHeight: true,
          title: 'No node selected',
        },
        'Click on a node to inspect its materializer plan.',
      );
    }

    if (!chunkQueries.ok) {
      return m(
        EmptyState,
        {icon: 'warning', fillHeight: true, title: 'Incomplete query'},
        chunkQueries.error,
      );
    }

    const chunks = chunkQueries.chunks;
    const outChunk = chunks[chunks.length - 1];
    // The output chunk is a table only when it's materialized.
    const tables = materializeOutput ? chunks : chunks.slice(0, -1);

    const cards: m.Children[] = [];

    if (tables.length > 0) {
      cards.push(
        m(
          Callout,
          {icon: 'info', className: 'pf-spag-mat-note'},
          'Cache status and timings reflect the most recent materialization run.',
        ),
      );
    }

    for (const chunk of tables) {
      const bodySql = withIncludes(chunk.includes, chunk.sql);
      const nodeIds =
        chunkPlan.chunks.find((c) => c.rootId === chunk.rootId)?.nodeIds ?? [];
      const meta =
        nodeIds.length > 0 ? `nodes: ${nodeIds.join(', ')}` : undefined;

      const outcome = lastRunOutcomes.get(chunk.hash);
      const badges: m.Children[] = [];
      if (outcome?.kind === 'building') {
        badges.push(m(Card.Badge, {variant: 'miss', label: 'BUILDING'}));
      } else if (outcome?.kind === 'hit') {
        badges.push(m(Card.Time, {label: 'CACHED'}));
      } else if (outcome?.kind === 'built') {
        badges.push(m(Card.Time, {label: `${outcome.timeMs.toFixed(1)}ms`}));
      } else if (outcome?.kind === 'skipped') {
        badges.push(m(Card.Time, {label: 'SKIPPED'}));
      }
      badges.push(
        m(CopyToClipboardButton, {
          textToCopy: bodySql,
          variant: ButtonVariant.Minimal,
          title: 'Copy SQL',
        }),
      );

      cards.push(
        m(Card, {
          dataHash: chunk.hash,
          icon: 'table_chart',
          hue: 204,
          className: classNames(
            this.highlightedHash === chunk.hash && 'pf-spag-card--highlighted',
          ),
          title: chunk.hash,
          meta,
          subheader: this.renderDepsRow('depends on', chunk.inputs),
          content: bodySql,
          badges,
        }),
      );
    }

    const finalSql =
      output === undefined
        ? ''
        : output.ok
          ? withIncludes(output.includes, output.sql)
          : `-- ${output.error}`;
    const finalDeps = materializeOutput
      ? outChunk
        ? [outChunk.hash]
        : []
      : (outChunk?.inputs ?? []);

    cards.push(
      m(Card, {
        icon: 'output',
        hue: 145,
        title: 'Output',
        meta: language,
        subheader: this.renderDepsRow('reads from', finalDeps),
        content: finalSql,
        badges: [
          m(CopyToClipboardButton, {
            textToCopy: finalSql,
            variant: ButtonVariant.Minimal,
            title: 'Copy SQL',
          }),
        ],
      }),
    );

    return m('.pf-spag-mat-tab', cards);
  }
}
