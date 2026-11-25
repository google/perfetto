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

import './kit.scss';
import m from 'mithril';
import {classNames} from '../../../../ui/src/base/classnames';
import {Button, ButtonVariant} from '../../../../ui/src/widgets/button';
import {Callout} from '../../../../ui/src/widgets/callout';
import {Intent} from '../../../../ui/src/widgets/common';
import {Icon} from '../../../../ui/src/widgets/icon';

export interface BlockAction {
  readonly label: string;
  readonly onclick: () => void;
  readonly icon?: string;
  readonly intent?: Intent;
}

export interface PageFact {
  readonly icon: string;
  readonly text: string;
}

// Inline `backtick` formatting for process/thread/file names inside prose.
function renderProse(text: string): m.Children {
  return text
    .split('`')
    .map((part, i) => (i % 2 === 1 ? m('code', part) : part));
}

function renderAnswerPoints(
  answer: string | readonly (string | undefined)[],
): m.Children {
  if (typeof answer === 'string') return renderProse(answer);
  const shown = answer.filter((p): p is string => Boolean(p));
  if (shown.length === 0) return undefined;
  if (shown.length === 1) return m('span', renderProse(shown[0]!));
  return m(
    'ul.pf-prod-points',
    shown.map((p) => m('li', renderProse(p))),
  );
}

// Elevated page container matching lat's page shell: secondary backdrop,
// centered white card, crisp header with title + inline facts + actions.
export function pageShell(
  attrs: {
    readonly title: m.Children;
    readonly facts?: ReadonlyArray<PageFact>;
    readonly actions?: m.Children;
  },
  body: m.Children,
): m.Child {
  return m(
    '.pf-prod-page',
    m('.pf-prod-page__inner', [
      m('.pf-prod-page__header', [
        m('h1.pf-prod-page__title', attrs.title),
        attrs.facts &&
          attrs.facts.length > 0 &&
          m(
            '.pf-prod-page__facts',
            attrs.facts.map((f) =>
              m('span.pf-prod-page__fact', [m(Icon, {icon: f.icon}), f.text]),
            ),
          ),
        attrs.actions !== undefined &&
          m('.pf-prod-page__actions', attrs.actions),
      ]),
      body,
    ]),
  );
}

// A question about the trace, its answer callout, and the evidence for it,
// with links to go deeper beside the question. Rendered as an elevated card.
export function questionBlock(
  attrs: {
    readonly question: string;
    readonly description?: string;
    readonly answer?: string | readonly (string | undefined)[];
    readonly answerIntent?: Intent;
    readonly actions?: ReadonlyArray<BlockAction>;
  },
  body?: m.Children,
): m.Child {
  const answerContent =
    attrs.answer !== undefined ? renderAnswerPoints(attrs.answer) : undefined;
  const intent = attrs.answerIntent ?? Intent.Primary;
  return m('section.pf-prod-block', [
    m('.pf-prod-block__header', [
      m('.pf-prod-block__q', attrs.question),
      attrs.actions !== undefined &&
        attrs.actions.length > 0 &&
        m(
          '.pf-prod-block__actions',
          attrs.actions.map((a) =>
            m(Button, {
              label: a.label,
              ...(a.icon !== undefined
                ? {icon: a.icon}
                : {rightIcon: 'arrow_forward'}),
              intent: a.intent,
              variant: ButtonVariant.Minimal,
              onclick: a.onclick,
            }),
          ),
        ),
    ]),
    attrs.description !== undefined &&
      m('.pf-prod-block__desc', attrs.description),
    answerContent !== undefined &&
      m(
        Callout,
        {
          icon:
            intent === Intent.Danger
              ? 'error'
              : intent === Intent.Warning
                ? 'warning'
                : 'lightbulb',
          intent,
          className: classNames(
            'pf-prod-block__callout',
            intent === Intent.Warning && 'pf-prod-block__callout--warn',
            intent === Intent.Danger && 'pf-prod-block__callout--danger',
          ),
        },
        answerContent,
      ),
    body !== undefined && m('.pf-prod-block__body', body),
  ]);
}

// One headline number, styled like lat's stat tiles.
export function stat(
  label: string,
  value: string,
  sub?: m.Children,
  tone?: 'ok' | 'warn' | 'bad',
): m.Child {
  return m(
    '.pf-prod-stat',
    {className: tone ? `pf-prod-stat--${tone}` : undefined},
    [
      m('.pf-prod-stat__label', label),
      m('.pf-prod-stat__value', value),
      sub !== undefined && m('.pf-prod-stat__sub', sub),
    ],
  );
}

export interface EvidenceBarPart {
  readonly label: string;
  readonly value: number;
  readonly display: string;
  readonly color?: string;
}

export interface EvidenceRow {
  readonly label: string;
  readonly parts: ReadonlyArray<EvidenceBarPart>;
  readonly total: number;
}

export interface AreaCardAttrs {
  readonly id: string;
  readonly title: string;
  readonly icon: string;
  readonly color: string;
  readonly headline: string;
  readonly headlineSub?: string;
  // 0..1 intensity bar on the right of the card header.
  readonly barFrac?: number;
  readonly quiet: boolean;
  readonly quietSummary: string;
  readonly finding?: string;
  readonly evidence?: ReadonlyArray<EvidenceRow>;
  readonly open: boolean;
  readonly onToggle: () => void;
  readonly onOpenTab: () => void;
  readonly openLabel?: string;
  readonly moreContent?: () => m.Children;
}

// Ranked resource card modeled on lat's .pf-lat-section: colored top border,
// bold headline metric, right-aligned bar, finding, compact evidence rows,
// smooth slide-out for more evidence, and quiet areas collapsed to one line.
export function areaCard(attrs: AreaCardAttrs): m.Child {
  const barPct = Math.max(0, Math.min(100, (attrs.barFrac ?? 0) * 100));
  if (attrs.quiet) {
    return m(
      '.pf-prod-section.pf-prod-section--small',
      {
        className: classNames(attrs.open && 'pf-prod-section--open'),
        style: {'--pf-prod-card-color': attrs.color},
      },
      [
        m('.pf-prod-section__head', [
          attrs.moreContent !== undefined &&
            m(Button, {
              icon: attrs.open ? 'expand_more' : 'chevron_right',
              variant: ButtonVariant.Minimal,
              compact: true,
              title: attrs.open ? 'Collapse details' : 'Expand details',
              onclick: attrs.onToggle,
            }),
          m('.pf-prod-section__name', attrs.title),
          m('.pf-prod-section__quiet-text', attrs.quietSummary),
          m('.pf-prod-section__quiet-action', [
            m(Button, {
              label: attrs.openLabel ?? `Open ${attrs.title}`,
              rightIcon: 'arrow_forward',
              variant: ButtonVariant.Minimal,
              compact: true,
              onclick: attrs.onOpenTab,
            }),
          ]),
        ]),
        attrs.moreContent !== undefined &&
          m(
            '.pf-prod-section__slide',
            m(
              '.pf-prod-section__inner',
              attrs.open && m('.pf-prod-section__more', attrs.moreContent()),
            ),
          ),
      ],
    );
  }

  return m(
    '.pf-prod-section',
    {
      className: classNames(attrs.open && 'pf-prod-section--open'),
      style: {'--pf-prod-card-color': attrs.color},
    },
    [
      m('.pf-prod-section__head', [
        m('.pf-prod-section__name', attrs.title),
        m('.pf-prod-section__share', attrs.headline),
        attrs.headlineSub && m('.pf-prod-section__sub', attrs.headlineSub),
        attrs.barFrac !== undefined &&
          m(
            '.pf-prod-section__bar',
            m('div', {style: {width: `${Math.max(2, barPct)}%`}}),
          ),
      ]),
      attrs.finding &&
        m('.pf-prod-section__finding', renderProse(attrs.finding)),
      attrs.evidence &&
        attrs.evidence.length > 0 &&
        m(
          '.pf-prod-section__rows',
          attrs.evidence.map((row) => renderEvidenceRow(row)),
        ),
      m('.pf-prod-section__foot', [
        attrs.moreContent !== undefined
          ? m(Button, {
              label: attrs.open ? 'Less evidence' : 'More evidence',
              icon: attrs.open ? 'expand_less' : 'expand_more',
              variant: ButtonVariant.Minimal,
              onclick: attrs.onToggle,
            })
          : m('span'),
        m(Button, {
          label: attrs.openLabel ?? `Open ${attrs.title}`,
          rightIcon: 'arrow_forward',
          variant: ButtonVariant.Outlined,
          onclick: attrs.onOpenTab,
        }),
      ]),
      attrs.moreContent !== undefined &&
        m(
          '.pf-prod-section__slide',
          m(
            '.pf-prod-section__inner',
            attrs.open && m('.pf-prod-section__more', attrs.moreContent()),
          ),
        ),
    ],
  );
}

function renderEvidenceRow(row: EvidenceRow): m.Child {
  const denom =
    row.total > 0
      ? row.total
      : row.parts.reduce((acc, p) => acc + Math.max(0, p.value), 0);
  if (denom <= 0) return undefined;
  const top = row.parts.slice(0, 3);
  const topSum = top.reduce((acc, p) => acc + Math.max(0, p.value), 0);
  const rest = Math.max(0, denom - topSum);
  return m('.pf-prod-ev', [
    m('.pf-prod-ev__label', row.label),
    m('.pf-prod-ev__bar', [
      top.map((p, idx) =>
        m('.pf-prod-ev__part', {
          title: `${p.label}: ${p.display}`,
          style: {
            width: `${Math.max(1.5, (100 * p.value) / denom)}%`,
            background: p.color,
            opacity: p.color ? '1' : String(Math.max(0.35, 1 - idx * 0.28)),
          },
        }),
      ),
      rest > 0 &&
        m('.pf-prod-ev__part.pf-prod-ev__part--rest', {
          style: {width: `${(100 * rest) / denom}%`},
        }),
    ]),
    m(
      '.pf-prod-ev__names',
      top.slice(0, 2).map((p, idx) => [
        idx > 0 && ' · ',
        m('code', p.label),
        ' ',
        m('span.pf-prod-ev__share', p.display),
      ]),
    ),
  ]);
}

// Specialist handoff / scope boundary rendered with Perfetto's Callout widget.
export function limitCallout(attrs: {
  readonly badge: string;
  readonly title: string;
  readonly body: string;
  readonly actionLabel?: string;
  readonly onAction?: () => void;
  readonly secondaryLabel?: string;
  readonly onSecondary?: () => void;
}): m.Child {
  return m(
    Callout,
    {
      icon: attrs.onAction ? 'open_in_new' : 'info',
      intent: Intent.None,
      className: 'pf-prod-boundary',
    },
    m('.pf-prod-boundary__row', [
      m('.pf-prod-boundary__copy', [
        m('strong', renderProse(attrs.title)),
        m('div.pf-prod-boundary__body', renderProse(attrs.body)),
      ]),
      (attrs.actionLabel || attrs.secondaryLabel) &&
        m('.pf-prod-boundary__actions', [
          attrs.actionLabel &&
            attrs.onAction &&
            m(Button, {
              label: attrs.actionLabel,
              rightIcon: 'open_in_new',
              intent: Intent.Primary,
              variant: ButtonVariant.Outlined,
              onclick: attrs.onAction,
            }),
          attrs.secondaryLabel &&
            attrs.onSecondary &&
            m(Button, {
              label: attrs.secondaryLabel,
              rightIcon: 'arrow_forward',
              variant: ButtonVariant.Minimal,
              onclick: attrs.onSecondary,
            }),
        ]),
    ]),
  );
}

// ---------------------------------------------------------------------------
// lat-style collapsible section panels, expandable ribbon rows & disclosures
// ---------------------------------------------------------------------------

export interface CollapsiblePanelAttrs {
  readonly title: string;
  readonly icon?: string;
  readonly color?: string;
  readonly metric?: string;
  readonly metricSub?: string;
  readonly badge?: {
    readonly label: string;
    readonly tone?: 'ok' | 'warn' | 'bad' | 'muted';
  };
  readonly summary?: string;
  readonly open: boolean;
  readonly onToggle: () => void;
  readonly actions?: ReadonlyArray<BlockAction>;
  readonly quiet?: boolean;
}

// Structured forensic section panel modeled on lat's sliding sections:
// always shows title, headline metric, status pill, and 1-line summary in its
// header; clicking smoothly expands or collapses the detailed evidence body.
export function collapsiblePanel(
  attrs: CollapsiblePanelAttrs,
  body: () => m.Children,
): m.Child {
  return m(
    'section.pf-prod-cpanel',
    {
      className: classNames(
        attrs.open && 'pf-prod-cpanel--open',
        attrs.quiet && !attrs.open && 'pf-prod-cpanel--quiet',
      ),
      style: attrs.color ? {'--pf-prod-card-color': attrs.color} : undefined,
    },
    [
      m(
        '.pf-prod-cpanel__head',
        {
          role: 'button',
          tabindex: 0,
          'aria-expanded': attrs.open ? 'true' : 'false',
          onclick: attrs.onToggle,
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Enter' || e.key === ' ') {
              e.preventDefault();
              attrs.onToggle();
            }
          },
        },
        [
          m(Icon, {
            icon: attrs.open ? 'expand_more' : 'chevron_right',
            className: 'pf-prod-cpanel__chev',
          }),
          attrs.icon &&
            m(Icon, {icon: attrs.icon, className: 'pf-prod-cpanel__icon'}),
          m('.pf-prod-cpanel__titles', [
            m('.pf-prod-cpanel__top', [
              m('span.pf-prod-cpanel__title', attrs.title),
              attrs.metric && m('span.pf-prod-cpanel__metric', attrs.metric),
              attrs.metricSub &&
                m('span.pf-prod-cpanel__sub', attrs.metricSub),
              attrs.badge &&
                m(
                  `span.pf-prod-cpanel__badge.pf-prod-cpanel__badge--${attrs.badge.tone ?? 'muted'}`,
                  attrs.badge.label,
                ),
            ]),
            attrs.summary &&
              m('.pf-prod-cpanel__summary', renderProse(attrs.summary)),
          ]),
          attrs.actions &&
            attrs.actions.length > 0 &&
            m(
              '.pf-prod-cpanel__actions',
              {
                onclick: (e: Event) => e.stopPropagation(),
              },
              attrs.actions.map((a) =>
                m(Button, {
                  label: a.label,
                  ...(a.icon !== undefined
                    ? {icon: a.icon}
                    : {rightIcon: 'arrow_forward'}),
                  intent: a.intent,
                  variant: ButtonVariant.Outlined,
                  compact: true,
                  onclick: a.onclick,
                }),
              ),
            ),
        ],
      ),
      m(
        '.pf-prod-cpanel__slide',
        m(
          '.pf-prod-cpanel__inner',
          attrs.open && m('.pf-prod-cpanel__body', body()),
        ),
      ),
    ],
  );
}

export interface RibbonSegment {
  readonly label: string;
  readonly value: number;
  readonly display: string;
  readonly color: string;
}

export function ribbonLegend(
  items: ReadonlyArray<{readonly label: string; readonly color: string}>,
  hint?: string,
): m.Child {
  return m('.pf-prod-exp__legend', [
    items.map((it) =>
      m('span.pf-prod-exp__legend-item', [
        m('span.pf-prod-exp__swatch', {style: {background: it.color}}),
        it.label,
      ]),
    ),
    hint && m('span.pf-prod-exp__legend-hint', hint),
  ]);
}

function renderRibbonBar(
  segments: ReadonlyArray<RibbonSegment>,
  maxTotal: number,
  big = false,
): m.Child {
  const denom = Math.max(
    maxTotal,
    segments.reduce((s, p) => s + Math.max(0, p.value), 0),
    0.0001,
  );
  const total = segments.reduce((s, p) => s + Math.max(0, p.value), 0);
  const shown = segments.filter((s) => s.value > 0);
  return m(
    '.pf-prod-exp__bar',
    {className: classNames(big && 'pf-prod-exp__bar--big')},
    shown.map((s) => {
      const pct = (100 * s.value) / (big ? Math.max(total, 0.0001) : denom);
      return m(
        '.pf-prod-exp__seg',
        {
          title: `${s.label}: ${s.display}`,
          style: {
            width: `${Math.max(1.2, pct)}%`,
            background: s.color,
          },
        },
        big && pct >= 14 ? m('span', `${s.label} ${s.display}`) : undefined,
      );
    }),
  );
}

export interface ExpandableRowAttrs {
  readonly key: string;
  readonly open: boolean;
  readonly onToggle: () => void;
  readonly title: m.Children;
  readonly subtitle?: m.Children;
  readonly primaryStat: string;
  readonly secondaryStat?: string;
  readonly chips?: m.Children;
  readonly segments: ReadonlyArray<RibbonSegment>;
  readonly maxTotal: number;
  readonly accentColor?: string;
}

// Clickable forensic row modeled on lat/slowest.ts: shows a shared-scale
// multi-segment proportion ribbon, headline stats, and chips; clicking expands
// smoothly in-place to show deep component cards, sub-tables, and actions.
export function expandableRow(
  attrs: ExpandableRowAttrs,
  detail: () => m.Children,
): m.Child {
  return m(
    '.pf-prod-exp__item',
    {
      key: attrs.key,
      className: classNames(attrs.open && 'pf-prod-exp__item--open'),
      style: attrs.accentColor
        ? {'--pf-prod-exp-accent': attrs.accentColor}
        : undefined,
    },
    [
      m(
        '.pf-prod-exp__row',
        {
          role: 'button',
          tabindex: 0,
          'aria-expanded': attrs.open ? 'true' : 'false',
          onclick: attrs.onToggle,
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Enter' || e.key === ' ') {
              e.preventDefault();
              attrs.onToggle();
            }
          },
        },
        [
          m(Icon, {
            icon: attrs.open ? 'expand_more' : 'chevron_right',
            className: 'pf-prod-exp__chev',
          }),
          m('.pf-prod-exp__label', [
            m('.pf-prod-exp__title', attrs.title),
            attrs.subtitle !== undefined &&
              m('.pf-prod-exp__sub', attrs.subtitle),
          ]),
          m('.pf-prod-exp__stats', [
            m('span.pf-prod-exp__primary', attrs.primaryStat),
            attrs.secondaryStat &&
              m('span.pf-prod-exp__secondary', attrs.secondaryStat),
            attrs.chips,
          ]),
          renderRibbonBar(attrs.segments, attrs.maxTotal, false),
        ],
      ),
      attrs.open &&
        m('.pf-prod-exp__detail', [
          attrs.segments.some((s) => s.value > 0) &&
            renderRibbonBar(attrs.segments, attrs.maxTotal, true),
          detail(),
        ]),
    ],
  );
}

// Compact colored-border breakdown card inside an expanded row (modeled on
// lat's .pf-lat-slowest__part).
export function detailPartCard(attrs: {
  readonly color: string;
  readonly label: string;
  readonly value: string;
  readonly sub?: string;
  readonly detail?: m.Children;
}): m.Child {
  return m(
    '.pf-prod-exp__part',
    {style: {'--pf-prod-card-color': attrs.color}},
    [
      m('.pf-prod-exp__part-head', [
        m('span.pf-prod-exp__part-val', attrs.value),
        m('span.pf-prod-exp__part-name', attrs.label),
        attrs.sub && m('span.pf-prod-exp__part-sub', attrs.sub),
      ]),
      attrs.detail && m('.pf-prod-exp__part-detail', attrs.detail),
    ],
  );
}

// Collapsed-by-default disclosure bar for secondary bulk tables (modeled on
// lat/waits_view.ts moreBlock).
export function disclosureMore(
  attrs: {
    readonly open: boolean;
    readonly label: string;
    readonly openLabel?: string;
    readonly onToggle: () => void;
  },
  body: () => m.Children,
): m.Child {
  return m(
    '.pf-prod-more',
    {className: classNames(attrs.open && 'pf-prod-more--open')},
    [
      m(
        'button.pf-prod-more__btn',
        {type: 'button', onclick: attrs.onToggle},
        [
          m(Icon, {icon: attrs.open ? 'expand_less' : 'expand_more'}),
          m('span', attrs.open ? (attrs.openLabel ?? attrs.label) : attrs.label),
        ],
      ),
      m(
        '.pf-prod-more__slide',
        m(
          '.pf-prod-more__inner',
          attrs.open && m('.pf-prod-more__body', body()),
        ),
      ),
    ],
  );
}
