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
import {Anchor} from '../../widgets/anchor';
import {Chip} from '../../widgets/chip';
import type {Intent} from '../../widgets/common';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {Tree, TreeNode} from '../../widgets/tree';
import type {
  PropRow,
  PropSection,
  PropTarget,
  PropValue,
} from './ui_hierarchy_props';

// Renders property sections, the same way on both levels. Links call
// `onLink`.
export function renderPropSections(
  sections: ReadonlyArray<PropSection>,
  onLink: (target: PropTarget) => void,
): m.Children {
  return sections
    .filter((s) => s.rows.length > 0)
    .map((s) =>
      m('.pf-uih-props-section', [
        m('.pf-uih-section-title', [
          s.title,
          s.action !== undefined &&
            m(
              Anchor,
              {
                className: 'pf-uih-section-title__action',
                title: s.action.title,
                onclick: () =>
                  s.action !== undefined && onLink(s.action.target),
              },
              s.action.text,
            ),
        ]),
        m(Tree, {bordered: true}, renderRows(s.rows, onLink)),
      ]),
    );
}

// Rows with children expand on click; they're keyed by content so another
// selection's rows start collapsed.
function renderRows(
  rows: ReadonlyArray<PropRow>,
  onLink: (target: PropTarget) => void,
): m.Children {
  if (!rows.some((r) => r.children !== undefined)) {
    return rows.map((r) => renderRow(r, onLink));
  }
  return rows.map((r, i) =>
    r.children === undefined
      ? m(TreeNode, {
          key: `${i}|${r.label}|${valueKey(r.value)}`,
          left: renderLabel(r.label),
          right: renderValue(r.value, onLink),
        })
      : m(ExpandableRow, {
          key: `${i}|${r.label}|${valueKey(r.value)}`,
          row: r,
          onLink,
        }),
  );
}

interface ExpandableRowAttrs {
  readonly row: PropRow;
  readonly onLink: (target: PropTarget) => void;
}

// A row whose children are only rendered while it is expanded: detail
// trees can be large.
class ExpandableRow implements m.ClassComponent<ExpandableRowAttrs> {
  private expanded = false;

  view({attrs: {row, onLink}}: m.CVnode<ExpandableRowAttrs>): m.Children {
    return m(
      TreeNode,
      {
        left: renderLabel(row.label),
        right: renderValue(row.value, onLink),
        showCaret: true,
        collapsed: !this.expanded,
        onCollapseChanged: (collapsed: boolean) => {
          this.expanded = !collapsed;
        },
      },
      this.expanded && renderRows(row.children ?? [], onLink),
    );
  }
}

function valueKey(v: PropValue): string {
  switch (v.kind) {
    case 'text':
    case 'copy':
    case 'chip':
      return v.text;
    case 'links':
      return v.links.map((l) => l.text).join(',');
    case 'list':
      return v.items.join(',');
  }
}

// The name of the selected item, its kind and short facts.
export function renderPropsHeader(
  name: string,
  kind: string,
  meta: ReadonlyArray<string>,
  intent?: Intent,
): m.Children {
  return m('.pf-uih-props-header', [
    m('.pf-uih-props-header__name', name),
    m('.pf-uih-props-header__meta', [
      m(Chip, {label: kind, intent, compact: true}),
      meta.map((t) => m('span', t)),
    ]),
  ]);
}

// Rows that differ from the previous snapshot read "old \u2192 new", with
// "\u2013" for a missing side.
function renderRow(
  r: PropRow,
  onLink: (target: PropTarget) => void,
): m.Children {
  const d = r.diff;
  if (d === undefined) {
    return m(TreeNode, {
      left: renderLabel(r.label),
      right: renderValue(r.value, onLink),
    });
  }
  const none = m('span.pf-uih-diff__none', '\u2013');
  const before =
    d.kind === 'added'
      ? none
      : m(
          'span.pf-uih-diff__old',
          renderValue(d.kind === 'changed' ? d.previous : r.value, onLink),
        );
  const after = d.kind === 'removed' ? none : renderValue(r.value, onLink);
  return m(TreeNode, {
    left: renderLabel(r.label),
    right: m('span.pf-uih-diff', [before, m('span', '\u2192'), after]),
  });
}

function renderValue(
  v: PropValue,
  onLink: (target: PropTarget) => void,
): m.Children {
  switch (v.kind) {
    case 'text':
      return m(
        'span',
        {title: v.title, className: v.mono ? 'pf-uih-code' : ''},
        v.text,
      );
    case 'copy':
      return m('span.pf-uih-copyable', [
        m('span.pf-uih-copyable__text.pf-uih-code', v.text),
        m(CopyToClipboardButton, {textToCopy: v.text, compact: true}),
      ]);
    case 'links':
      return m(
        '.pf-uih-value-list',
        v.links.map((l) =>
          m(Anchor, {title: l.title, onclick: () => onLink(l.target)}, l.text),
        ),
      );
    case 'list':
      return m(
        '.pf-uih-value-list',
        v.items.map((t) => m('span', t)),
      );
    case 'chip':
      return m(Chip, {label: v.text, compact: true});
  }
}

// Long labels are cut by the stylesheet; the tooltip has the full text.
function renderLabel(label: string): m.Children {
  return m('span', {title: label}, label);
}
