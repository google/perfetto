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

// Top bar hosting the primary heap-dump selector and baseline pool
// controls. Hides when neither has anything to show.

import m from 'mithril';
import {Trace} from '../../public/trace';
import {Time} from '../../base/time';
import {Button, ButtonVariant} from '../../widgets/button';
import {MenuDivider, MenuItem, MenuTitle, PopupMenu} from '../../widgets/menu';
import {formatDuration} from '../../components/time_utils';
import * as queries from './queries';
import {HeapDumpDiffHeader, shouldShowBaselineHeader} from './header';
import {setSelfTraceBaseline} from './baseline/state';
import {HeapDumpExplorerSession} from './session';

interface TopBarAttrs {
  readonly trace: Trace;
  readonly session: HeapDumpExplorerSession;
  readonly onDumpChanged: () => void;
}

export class TopBar implements m.ClassComponent<TopBarAttrs> {
  view({attrs}: m.Vnode<TopBarAttrs>): m.Children {
    const hasPrimary =
      attrs.session.dumps.length > 1 && attrs.session.activeDump !== null;
    const hasBaseline = shouldShowBaselineHeader();
    // The HeapDumpDiffHeader always keeps a hidden file input mounted so
    // the Overview-tab CTA can fire it. Render it even when the visible
    // row collapses — it returns just the input in that case.
    if (!hasPrimary && !hasBaseline) {
      return m('div', {class: 'ah-top-bar ah-top-bar--hidden'}, [
        m(HeapDumpDiffHeader, {trace: attrs.trace}),
      ]);
    }
    return m(
      'div',
      {class: 'ah-top-bar'},
      hasPrimary ? renderPrimarySelector(attrs) : null,
      hasPrimary && hasBaseline
        ? m('span', {class: 'ah-top-bar__separator'}, '|')
        : null,
      m(HeapDumpDiffHeader, {trace: attrs.trace}),
    );
  }
}

function renderPrimarySelector(attrs: TopBarAttrs): m.Children {
  const allDumps = attrs.session.dumps;
  const active = attrs.session.activeDump!;
  const otherDumps = allDumps.filter((d) => d !== active);
  return [
    m('span', {class: 'ah-top-bar__label'}, 'Heap dump:'),
    m(
      PopupMenu,
      {
        trigger: m(Button, {
          label: processLabel(active),
          icon: 'memory',
          rightIcon: 'arrow_drop_down',
          variant: ButtonVariant.Outlined,
          compact: true,
        }),
      },
      [
        ...allDumps.map((d) =>
          m(MenuItem, {
            label: itemLabel(d, attrs.trace),
            active: d === active,
            onclick: () => {
              attrs.session.selectDump(d);
              attrs.onDumpChanged();
            },
          }),
        ),
        otherDumps.length > 0 ? m(MenuDivider) : null,
        otherDumps.length > 0
          ? m(MenuTitle, {label: 'Diff against this dump:'})
          : null,
        ...otherDumps.map((d) =>
          m(MenuItem, {
            label: itemLabel(d, attrs.trace),
            icon: 'difference',
            onclick: () =>
              setSelfTraceBaseline(
                attrs.trace.engine,
                attrs.trace.traceInfo.traceTitle,
                allDumps,
                d,
              ),
          }),
        ),
      ],
    ),
  ];
}

function processLabel(d: queries.HeapDump): string {
  // See header.ts:dumpProcessLabel — pid 0 means "unknown" for hprofs
  // without process metadata.
  const hasPid = d.pid !== null && d.pid !== 0;
  if (d.processName !== null && hasPid) {
    return `${d.processName} (pid ${d.pid})`;
  }
  if (d.processName !== null) return d.processName;
  if (hasPid) return `pid ${d.pid}`;
  return 'Java heap dump';
}

function itemLabel(d: queries.HeapDump, trace: Trace): string {
  const offset = Time.diff(Time.fromRaw(d.ts), trace.traceInfo.start);
  return `${processLabel(d)} — ${formatDuration(trace, offset)}`;
}
