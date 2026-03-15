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
import type {Engine} from '../../../trace_processor/engine';
import {Spinner} from '../../../widgets/spinner';
import type {InstanceRow} from '../types';
import {fmtSize} from '../format';
import {type NavFn, SortableTable, InstanceLink} from '../components';
import * as queries from '../queries';

interface FlamegraphObjectsViewAttrs {
  engine: Engine;
  navigate: NavFn;
  onBackToTimeline?: () => void;
  nodeName?: string;
  /** Comma-separated path hashes from the flamegraph selection. */
  pathHashes?: string;
  /** Whether the path hashes are from the dominator tree. */
  isDominator?: boolean;
}

function FlamegraphObjectsView(): m.Component<FlamegraphObjectsViewAttrs> {
  let rows: InstanceRow[] | null = null;
  let error: string | null = null;
  let alive = true;

  function fetchData(attrs: FlamegraphObjectsViewAttrs) {
    rows = null;
    error = null;

    if (!attrs.pathHashes) {
      error = 'no-selection';
      return;
    }

    queries
      .getObjectsByFlamegraphSelection(
        attrs.engine,
        attrs.pathHashes,
        attrs.isDominator ?? true,
      )
      .then((r) => {
        if (!alive) return;
        rows = r;
        m.redraw();
      })
      .catch((e) => {
        if (!alive) return;
        error = String(e);
        m.redraw();
      });
  }

  return {
    oninit(vnode) {
      fetchData(vnode.attrs);
    },
    onremove() {
      alive = false;
    },
    view(vnode) {
      const {navigate, nodeName, onBackToTimeline} = vnode.attrs;

      if (error) {
        return m('div', [
          nodeName
            ? m(
                'h2',
                {class: 'ah-view-heading'},
                'Flamegraph: ',
                m('span', {class: 'ah-mono'}, nodeName),
              )
            : null,
          m(
            'div',
            {class: 'ah-card ah-mb-3'},
            m(
              'p',
              'No flamegraph selection found. Select a node in the ',
              'flamegraph and choose "Open in Ahat" to see objects here.',
            ),
          ),
        ]);
      }

      if (!rows) {
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
      }

      if (rows.length === 0) {
        return m(
          'div',
          {class: 'ah-card ah-mb-3'},
          m('p', 'No objects found for this flamegraph selection.'),
        );
      }

      // Derive class name from first row for display.
      const className = rows[0].className;
      const uniqueClasses = new Set(rows.map((r) => r.className));

      return m('div', [
        m('div', {class: 'ah-heading-row'}, [
          m(
            'h2',
            {class: 'ah-view-heading'},
            nodeName
              ? ['Flamegraph: ', m('span', {class: 'ah-mono'}, nodeName)]
              : 'Flamegraph Objects',
          ),
          onBackToTimeline
            ? m(
                'button',
                {class: 'ah-download-link', onclick: onBackToTimeline},
                'Back to Timeline',
              )
            : null,
        ]),
        m('div', {class: 'ah-card--compact ah-mb-3'}, [
          m('div', {class: 'ah-info-grid--compact'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Class:'),
            m(
              'span',
              {class: 'ah-mono'},
              uniqueClasses.size === 1
                ? className
                : `${uniqueClasses.size} classes`,
            ),
            m('span', {class: 'ah-info-grid__label'}, 'Count:'),
            m('span', {class: 'ah-mono'}, rows.length.toLocaleString()),
          ]),
        ]),
        m(SortableTable, {
          columns: [
            {
              label: 'Size',
              align: 'right' as const,
              sortKey: (r: InstanceRow) => r.shallowJava + r.shallowNative,
              render: (r: InstanceRow) =>
                m(
                  'span',
                  {class: 'ah-mono'},
                  fmtSize(r.shallowJava + r.shallowNative),
                ),
            },
            {
              label: 'Retained',
              align: 'right' as const,
              sortKey: (r: InstanceRow) => r.retainedTotal,
              render: (r: InstanceRow) =>
                m('span', {class: 'ah-mono'}, fmtSize(r.retainedTotal)),
            },
            {
              label: 'Heap',
              render: (r: InstanceRow) => m('span', r.heap),
            },
            {
              label: 'Object',
              render: (r: InstanceRow) => m(InstanceLink, {row: r, navigate}),
            },
          ],
          data: rows,
          rowKey: (r: InstanceRow) => r.id,
        }),
      ]);
    },
  };
}

export default FlamegraphObjectsView;
