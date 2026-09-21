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
import {Gate} from '../../../base/mithril_utils';
import {TabStrip} from '../../../widgets/tab_strip';

export interface LazyTab {
  readonly key: string;
  readonly title: string;
  // Full hash URL (e.g. '#!/heapdump/objects') this tab navigates to.
  readonly href?: string;
  readonly onclick?: () => void;
}

export interface LazyTabsAttrs {
  readonly handles: readonly LazyTab[];
  readonly page: m.Children;
  readonly contentForKey: string;
  // Mirrors the height behaviour of the `Tabs` widget (`.pf-tabs`): the
  // container fills its parent and the content pane takes the remaining
  // space, scrolling if needed.
  readonly fillHeight?: boolean;
}

export function LazyTabs(): m.Component<LazyTabsAttrs> {
  const pageMap = new Map<string, m.Children>();
  return {
    view: ({attrs}) => {
      pageMap.set(attrs.contentForKey, attrs.page);
      return m(
        'div',
        {
          class: classNames(
            'pf-lazy-tabs',
            attrs.fillHeight && 'pf-lazy-tabs--fill-height',
          ),
        },
        [
          // TODO: TabStrip is deprecated (in favour of `Tabs`), but Tabs
          // eagerly renders all tab content, which defeats the point of
          // LazyTabs.
          m(TabStrip, {
            tabs: attrs.handles.map(({key, title, href}) => ({
              key,
              title,
              href,
            })),
            currentTabKey: attrs.contentForKey,
            onTabChange: (key) =>
              attrs.handles.find((handle) => handle.key === key)?.onclick?.(),
          }),
          m('div', {class: 'pf-lazy-tabs__content'}, [
            Array.from(pageMap.entries()).map(([key, page]) =>
              m(Gate, {open: key === attrs.contentForKey}, page),
            ),
          ]),
        ],
      );
    },
  };
}
