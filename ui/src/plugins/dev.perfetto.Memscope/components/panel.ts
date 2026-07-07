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

import './panel.scss';
import m from 'mithril';

// Panel — the workhorse "card" of the Memscope UI: a bordered, raised surface
// that groups a titled header with a body of content (a chart, a table, a strip
// of Billboards). Nearly every block on a Memscope page is a Panel, which is
// what gives the pages their consistent rhythm.
//
// Compose one from the two namespaced slots: a Panel.Header (a flat
// {title, subtitle, controls} shape — every call site wants the same layout, so
// it takes attrs rather than hand-assembled sub-slots) above a Panel.Body that
// holds the payload. See the example at the bottom of this file.

export interface PanelAttrs {
  readonly className?: string;
}

export class Panel implements m.ClassComponent<PanelAttrs> {
  view({attrs, children}: m.CVnode<PanelAttrs>): m.Children {
    return m('.pf-memscope-panel', {className: attrs.className}, children);
  }
}

export namespace Panel {
  export interface HeaderAttrs {
    readonly title: m.Children;
    readonly subtitle?: m.Children;
    readonly controls?: m.Children;
  }

  // The panel's header: a title row (title on the left, optional right-aligned
  // controls) above an optional subtitle line.
  export class Header implements m.ClassComponent<HeaderAttrs> {
    view({attrs}: m.CVnode<HeaderAttrs>): m.Children {
      return m(
        '.pf-memscope-panel__header',
        m(
          '.pf-memscope-panel__title-row',
          m('h2.pf-memscope-panel__title', attrs.title),
          attrs.controls !== undefined &&
            m('.pf-memscope-panel__controls', attrs.controls),
        ),
        attrs.subtitle !== undefined &&
          m('p.pf-memscope-panel__subtitle', attrs.subtitle),
      );
    }
  }

  export interface BodyAttrs {
    readonly className?: string;
  }

  // The content area below the header. Holds the panel's actual payload
  // (chart, table, billboards, free text).
  export class Body implements m.ClassComponent<BodyAttrs> {
    view({attrs, children}: m.CVnode<BodyAttrs>): m.Children {
      return m(
        '.pf-memscope-panel__body',
        {className: attrs.className},
        children,
      );
    }
  }
}

// === Example usage ===
//
// import {Button} from '../../../../widgets/button';
//
// m(Panel,
//   m(Panel.Header, {
//     title: 'Memory Overview',
//     subtitle: 'Heap usage across all processes',
//     controls: m(Button, {label: 'Refresh', icon: 'refresh', onclick: reload}),
//   }),
//   m(Panel.Body, m(HeapChart, {data})),
// );
