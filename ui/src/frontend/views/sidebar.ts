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

import './sidebar.scss';
import m from 'mithril';
import {assetSrc} from '../../base/assets';
import type {AppImpl} from '../../core/app_impl';
import {getCurrentChannel} from '../../core/channels';
import {raf} from '../../core/raf_scheduler';
import type {SidebarMenuItemInternal} from '../../core/sidebar_manager';
import {SCM_REVISION, VERSION} from '../../virtual/version';
import {SIDEBAR_SECTIONS, type SidebarSections} from '../../public/sidebar';
import {Icon} from '../../widgets/icon';
import {Animation} from '../animation';
import {EngineStatusBadge} from './engine_status_badge';
import {HiringBanner, shouldShowHiringBanner} from './hiring_banner';
import {ServiceWorkerStatusBadge} from './service_worker_status_badge';
import {SidebarSection} from './sidebar_section';

const GITILES_URL = 'https://github.com/google/perfetto';

export interface SidebarAttrs {
  readonly app: AppImpl;
}

export class Sidebar implements m.ClassComponent<SidebarAttrs> {
  private _redrawWhileAnimating = new Animation(() => raf.scheduleFullRedraw());

  view({attrs}: m.CVnode<SidebarAttrs>) {
    const app = attrs.app;
    const sidebar = app.sidebar;
    if (!sidebar.enabled) return null;
    return m(
      'nav.pf-sidebar',
      {
        class: sidebar.visible ? undefined : 'pf-sidebar--hidden',
        // 150 here matches --sidebar-timing in the css.
        // TODO(hjd): Should link to the CSS variable.
        ontransitionstart: (e: TransitionEvent) => {
          if (e.target !== e.currentTarget) return;
          this._redrawWhileAnimating.start(150);
        },
        ontransitionend: (e: TransitionEvent) => {
          if (e.target !== e.currentTarget) return;
          this._redrawWhileAnimating.stop();
        },
      },
      shouldShowHiringBanner(app) && m(HiringBanner),
      this.renderSidebarHeader(app),
      this.renderSidebarContent(app),
      this.renderSidebarFooter(app),
    );
  }

  private renderSidebarHeader(app: AppImpl) {
    const sidebar = app.sidebar;
    return m(
      `header.pf-sidebar__header`,
      {
        className: `pf-sidebar__header--${getCurrentChannel()}`,
      },
      m(`img[src=${assetSrc('assets/brand.png')}].pf-sidebar__brand`),
      app.embedder.brandingBadge &&
        m(
          'span.pf-sidebar__branding-badge',
          {style: {color: app.embedder.brandingBadge.color}},
          app.embedder.brandingBadge.image
            ? m('img.pf-sidebar__branding-badge-img', {
                src: app.embedder.brandingBadge.image,
              })
            : app.embedder.brandingBadge.icon &&
                m(Icon, {
                  icon: app.embedder.brandingBadge.icon,
                  className: 'pf-sidebar__branding-badge-icon',
                }),
          app.embedder.brandingBadge.text,
        ),
      m(
        'button.pf-sidebar-button',
        {
          onclick: () => sidebar.toggleVisibility(),
          title: sidebar.visible ? 'Hide sidebar' : 'Show sidebar',
        },
        m(Icon, {icon: 'menu'}),
      ),
    );
  }

  private renderSidebarContent(app: AppImpl) {
    return m(
      '.pf-sidebar__content',
      (Object.keys(SIDEBAR_SECTIONS) as SidebarSections[]).map((sectionId) =>
        this.renderSection(app, sectionId),
      ),
    );
  }

  private renderSidebarFooter(app: AppImpl) {
    return m(
      '.pf-sidebar__footer',
      m(EngineStatusBadge, {app}),
      m(ServiceWorkerStatusBadge, {app}),
      m(
        '.pf-sidebar__version.pf-test-volatile',
        m(
          'a',
          {
            href: `${GITILES_URL}/tree/${SCM_REVISION}/ui`,
            title: `Channel: ${getCurrentChannel()}`,
            target: '_blank',
          },
          VERSION,
        ),
      ),
    );
  }

  private renderSection(app: AppImpl, sectionId: SidebarSections) {
    const section = SIDEBAR_SECTIONS[sectionId];
    const trace = app.trace;

    const items: SidebarMenuItemInternal[] = app.sidebar.menuItems
      .valuesAsArray()
      .filter((item) => item.section === sectionId);

    let defaultCollapsed = false;
    let leading: m.Children;
    switch (sectionId) {
      case 'current_trace':
        if (trace !== undefined) {
          const traceTitle = trace.traceInfo.traceTitle;
          const fileSizeBytes = trace.traceInfo.fileSizeBytes;

          if (traceTitle) {
            leading = m(
              'span.pf-sidebar__trace-file-name',
              traceTitle,
              fileSizeBytes !== undefined &&
                ` (${Math.ceil(fileSizeBytes / 1e6)} MB)`,
            );
          }
        }
        break;
      case 'convert_trace':
        defaultCollapsed = true;
        break;
      case 'support':
        defaultCollapsed = true;
        break;
    }

    return m(SidebarSection, {
      app,
      title: section.title,
      summary: section.summary,
      items,
      defaultCollapsed,
      leading,
    });
  }
}
