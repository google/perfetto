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

import './tabs_nav.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import type {AppImpl} from '../core/app_impl';
import {Icon} from '../widgets/icon';

// Renders the brand and tabs of the 'tabs' navigation style (see
// Navigation in core/embedder/embedder.ts), or nothing for other styles.
export function renderTabsNav(app: AppImpl): m.Children {
  const nav = app.embedder.navigation;
  if (nav?.kind !== 'tabs') return undefined;
  const page = app.getCurrentRoute().page;
  return m(
    '.pf-tabs-nav',
    m(
      'a.pf-tabs-nav__brand',
      {href: '#!/'},
      nav.brand.icon !== undefined && m(Icon, {icon: nav.brand.icon}),
      nav.brand.text,
    ),
    nav.tabs
      .filter((t) => t.isVisible?.(app) ?? true)
      .map((t) =>
        m(
          'a.pf-tabs-nav__tab',
          {
            href: t.href?.() ?? `#!${t.page}`,
            className: classNames(
              page === t.page && 'pf-tabs-nav__tab--active',
            ),
          },
          t.label,
        ),
      ),
  );
}
