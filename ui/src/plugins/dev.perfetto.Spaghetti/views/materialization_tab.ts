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
import {formatLogicalPlan, formatSql, type SqlDialect} from '../ir';
import type {MaterializationPlan, StepRunOutcome} from '../materialization';
import './materialization_tab.scss';

export interface MaterializationTabAttrs {
  readonly matPlan: MaterializationPlan | undefined;
  readonly dialect: SqlDialect;
  readonly lastRunOutcomes: ReadonlyMap<string, StepRunOutcome>;
  readonly lastIncludesTimeMs: number | undefined;
  readonly activeNodeId: string | undefined;
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
      matPlan,
      dialect,
      lastRunOutcomes,
      lastIncludesTimeMs,
      activeNodeId,
    } = attrs;

    if (!matPlan) {
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

    const stepByHash = new Map(matPlan.steps.map((s) => [s.hash, s]));
    const cards: m.Children[] = [];

    if (matPlan.steps.length > 0 || matPlan.includes.length > 0) {
      cards.push(
        m(
          Callout,
          {icon: 'info', className: 'pf-spag-mat-note'},
          'Cache status and timings reflect the most recent materialization run.',
        ),
      );
    }

    if (matPlan.includes.length > 0) {
      const includeSql = matPlan.includes
        .map((inc) => `INCLUDE PERFETTO MODULE ${inc};`)
        .join('\n');
      cards.push(
        m(Card, {
          icon: 'auto_stories',
          hue: 272,
          title: 'Includes',
          meta: `${matPlan.includes.length} ${matPlan.includes.length === 1 ? 'module' : 'modules'}`,
          content: includeSql,
          badges: [
            lastIncludesTimeMs !== undefined &&
              m(Card.Time, {label: `${lastIncludesTimeMs.toFixed(1)}ms`}),
            m(CopyToClipboardButton, {
              textToCopy: includeSql,
              variant: ButtonVariant.Minimal,
              title: 'Copy SQL',
            }),
          ],
        }),
      );
    }

    for (const step of matPlan.steps) {
      const res = formatLogicalPlan(step.plan, dialect, {
        includeModuleHeaders: false,
      });
      const bodySql = res.ok
        ? `CREATE PERFETTO TABLE ${step.hash} AS\n${res.sql}`
        : `-- Unsupported in Standard SQL: ${res.error}`;

      const meta =
        step.nodeIds.length > 0
          ? `nodes: ${step.nodeIds.join(', ')}`
          : undefined;

      const outcome = lastRunOutcomes.get(step.hash);
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
          dataHash: step.hash,
          icon: 'table_chart',
          hue: 204,
          className: classNames(
            this.highlightedHash === step.hash && 'pf-spag-card--highlighted',
          ),
          title: step.hash,
          meta,
          subheader: this.renderDepsRow('depends on', step.deps),
          content: bodySql,
          badges,
        }),
      );
    }

    const finalDeps = [
      ...new Set(
        matPlan.finalRead.nodes.flatMap((n) =>
          n.op.kind === 'scan' && stepByHash.has(n.op.table)
            ? [n.op.table]
            : [],
        ),
      ),
    ];

    const singleScan =
      matPlan.finalRead.nodes.length === 1 &&
      matPlan.finalRead.nodes[0]?.op.kind === 'scan'
        ? matPlan.finalRead.nodes[0].op.table
        : undefined;
    const finalRes = formatLogicalPlan(matPlan.finalRead, dialect, {
      includeModuleHeaders: false,
    });
    const fallbackSql = formatSql(matPlan.finalRead, {
      includeModuleHeaders: false,
    });
    const finalSql =
      singleScan !== undefined
        ? `SELECT * FROM ${singleScan}`
        : finalRes.ok
          ? finalRes.sql
          : fallbackSql.ok
            ? fallbackSql.sql
            : `-- ${finalRes.error}`;

    cards.push(
      m(Card, {
        icon: 'output',
        hue: 145,
        title: 'Output',
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
