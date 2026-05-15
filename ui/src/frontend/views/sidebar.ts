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
import {assetSrc} from '../../base/assets';
import {AppImpl} from '../../core/app_impl';
import {getCurrentChannel} from '../../core/channels';
import {isMetatracingEnabled} from '../../core/metatracing';
import {raf} from '../../core/raf_scheduler';
import type {SidebarMenuItemInternal} from '../../core/sidebar_manager';
import type {TraceImpl} from '../../core/trace_impl';
import {SCM_REVISION, VERSION} from '../../gen/perfetto_version';
import type {App} from '../../public/app';
import {SIDEBAR_SECTIONS, type SidebarSections} from '../../public/sidebar';
import {Icon} from '../../widgets/icon';
import {Animation} from '../animation';
import {toggleHelp} from '../help_modal';
import {
  convertTraceToJson,
  convertTraceToSystrace,
  downloadTrace,
  toggleMetatrace,
} from '../trace_actions';
import {shareTrace} from '../trace_share_utils';
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
          items.push(...getCurrentTraceItems(trace));
          if (trace.traceInfo.traceTitle) {
            leading = m(
              'span.pf-sidebar__trace-file-name',
              trace.traceInfo.traceTitle,
            );
          }
        }
        break;
      case 'convert_trace':
        if (trace !== undefined) {
          items.push(...getConvertTraceItems(trace));
        }
        defaultCollapsed = true;
        break;
      case 'support':
        items.push(...getSupportGlobalItems(app));
        if (trace !== undefined) {
          items.push(...getSupportTraceItems(trace));
        }
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

// TODO(primiano): The items below should be moved to dedicated
// plugins (most of this really belongs to core_plugins/commands/index.ts).
// For now keeping everything here as splitting these require moving some
// functions like share_trace() out of core, splitting out permalink, etc.

// Returns menu items for the 'current_trace' section.
function getCurrentTraceItems(trace: TraceImpl): SidebarMenuItemInternal[] {
  const items: SidebarMenuItemInternal[] = [];
  const downloadDisabled = trace.traceInfo.downloadable
    ? false
    : 'Cannot download external trace';

  if (AppImpl.instance.isInternalUser) {
    items.push({
      id: 'perfetto.ShareTrace',
      section: 'current_trace',
      sortOrder: 50,
      text: 'Share',
      action: async () => await shareTrace(trace),
      icon: 'share',
    });
  }

  items.push({
    id: 'perfetto.DownloadTrace',
    section: 'current_trace',
    sortOrder: 51,
    text: 'Download',
    action: () => downloadTrace(trace),
    icon: 'file_download',
    disabled: downloadDisabled,
  });

  return items;
}

// Returns menu items for the 'convert_trace' section.
function getConvertTraceItems(trace: TraceImpl): SidebarMenuItemInternal[] {
  const items: SidebarMenuItemInternal[] = [];
  const downloadDisabled = trace.traceInfo.downloadable
    ? false
    : 'Cannot download external trace';

  items.push({
    id: 'perfetto.ConvertToJson',
    section: 'convert_trace',
    text: 'Convert to .json',
    action: async () => await convertTraceToJson(trace),
    icon: 'file_download',
    disabled: downloadDisabled,
  });

  if (trace.traceInfo.hasFtrace) {
    items.push({
      id: 'perfetto.ConvertToSystrace',
      section: 'convert_trace',
      text: 'Convert to .systrace',
      action: async () => await convertTraceToSystrace(trace),
      icon: 'file_download',
      disabled: downloadDisabled,
    });
  }

  return items;
}

// Returns global menu items for the 'support' section (always visible).
function getSupportGlobalItems(app: App): SidebarMenuItemInternal[] {
  // TODO(primiano): The Open file / Open with legacy entries are registered by
  // the 'perfetto.CoreCommands' plugin. These built-in items should move there too.
  return [
    {
      id: 'perfetto.KeyboardShortcuts',
      section: 'support',
      text: 'Keyboard shortcuts',
      action: toggleHelp,
      icon: 'help',
    },
    {
      id: 'perfetto.Documentation',
      section: 'support',
      text: 'Documentation',
      href: 'https://perfetto.dev/docs',
      icon: 'find_in_page',
    },
    {
      id: 'perfetto.ReportBug',
      section: 'support',
      sortOrder: 4,
      text: 'Report a bug',
      href: getBugReportUrl(app),
      icon: 'bug_report',
    },
  ];
}

function getBugReportUrl(app: App): string {
  if (app.isInternalUser) {
    return 'https://goto.google.com/perfetto-ui-bug';
  } else {
    return 'https://github.com/google/perfetto/issues/new';
  }
}

// Returns trace-specific menu items for the 'support' section.
function getSupportTraceItems(trace: TraceImpl): SidebarMenuItemInternal[] {
  return [
    {
      id: 'perfetto.Metatrace',
      section: 'support',
      sortOrder: 5,
      text: () =>
        isMetatracingEnabled() ? 'Finalize metatrace' : 'Record metatrace',
      action: () => toggleMetatrace(trace.engine),
      icon: () => (isMetatracingEnabled() ? 'download' : 'fiber_smart_record'),
    },
  ];
}
