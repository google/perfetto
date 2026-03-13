// Copyright (C) 2018 The Android Open Source Project
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
import {assetSrc} from '../base/assets';
import {classNames} from '../base/classnames';
import {formatHotkey} from '../base/hotkeys';
import {exists, getOrCreate} from '../base/utils';
import {AppImpl} from '../core/app_impl';
import {getCurrentChannel} from '../core/channels';
import {featureFlags} from '../core/feature_flags';
import {isMetatracingEnabled} from '../core/metatracing';
import {raf} from '../core/raf_scheduler';
import {Router} from '../core/router';
import {SidebarMenuItemInternal} from '../core/sidebar_manager';
import {OptionalTraceImplAttrs, TraceImpl} from '../core/trace_impl';
import {SCM_REVISION, VERSION} from '../gen/perfetto_version';
import {Command} from '../public/command';
import {SIDEBAR_SECTIONS, SidebarSections} from '../public/sidebar';
import {EngineMode} from '../trace_processor/engine';
import {Button} from '../widgets/button';
import {Icon} from '../widgets/icon';
import {Icons} from '../base/semantic_icons';
import {showModal} from '../widgets/modal';
import {Spinner} from '../widgets/spinner';
import {Animation} from './animation';
import {toggleHelp} from './help_modal';
import {
  convertTraceToJson,
  convertTraceToSystrace,
  downloadTrace,
  openCurrentTraceWithOldUI,
  toggleMetatrace,
} from './trace_actions';
import {shareTrace} from './trace_share_utils';

const GITILES_URL = 'https://github.com/google/perfetto';

function getBugReportUrl(): string {
  if (AppImpl.instance.isInternalUser) {
    return 'https://goto.google.com/perfetto-ui-bug';
  } else {
    return 'https://github.com/google/perfetto/issues/new';
  }
}

const HIRING_BANNER_FLAG = featureFlags.register({
  id: 'showHiringBanner',
  name: 'Show hiring banner',
  description: 'Show the "We\'re hiring" banner link in the side bar.',
  defaultValue: false,
});

function shouldShowHiringBanner(): boolean {
  return AppImpl.instance.isInternalUser && HIRING_BANNER_FLAG.get();
}

class EngineRPCWidget implements m.ClassComponent<OptionalTraceImplAttrs> {
  view({attrs}: m.CVnode<OptionalTraceImplAttrs>) {
    let cssClass = '';
    let title = 'Number of pending SQL queries';
    let label: string;
    let failed = false;
    let mode: EngineMode | undefined;

    const engine = attrs.trace?.engine;
    if (engine !== undefined) {
      mode = engine.mode;
      if (engine.failed !== undefined) {
        cssClass += '.pf-sidebar__dbg-info-square--red';
        title = 'Query engine crashed\n' + engine.failed;
        failed = true;
      }
    }

    // If we don't have an engine yet, guess what will be the mode that will
    // be used next time we'll create one. Even if we guess it wrong (somehow
    // trace_controller.ts takes a different decision later, e.g. because the
    // RPC server is shut down after we load the UI and cached httpRpcState)
    // this will eventually become  consistent once the engine is created.
    if (mode === undefined) {
      if (
        AppImpl.instance.httpRpc.httpRpcAvailable &&
        AppImpl.instance.httpRpc.newEngineMode === 'USE_HTTP_RPC_IF_AVAILABLE'
      ) {
        mode = 'HTTP_RPC';
      } else {
        mode = 'WASM';
      }
    }

    if (mode === 'HTTP_RPC') {
      cssClass += '.pf-sidebar__dbg-info-square--green';
      label = 'RPC';
      title += '\n(Query engine: native accelerator over HTTP+RPC)';
    } else {
      label = 'WSM';
      title += '\n(Query engine: built-in WASM)';
    }

    const numReqs = attrs.trace?.engine.numRequestsPending ?? 0;
    return m(
      `.pf-sidebar__dbg-info-square${cssClass}`,
      {title},
      m('div', label),
      m('div', `${failed ? 'FAIL' : numReqs}`),
    );
  }
}

const ServiceWorkerWidget: m.Component = {
  view() {
    let cssClass = '';
    let title = 'Service Worker: ';
    let label = 'N/A';
    const ctl = AppImpl.instance.serviceWorkerController;
    if (!('serviceWorker' in navigator)) {
      label = 'N/A';
      title += 'not supported by the browser (requires HTTPS)';
    } else if (ctl.bypassed) {
      label = 'OFF';
      cssClass = '.pf-sidebar__dbg-info-square--red';
      title += 'Bypassed, using live network. Double-click to re-enable';
    } else if (ctl.installing) {
      label = 'UPD';
      cssClass = '.pf-sidebar__dbg-info-square--amber';
      title += 'Installing / updating ...';
    } else if (!navigator.serviceWorker.controller) {
      label = 'N/A';
      title += 'Not available, using network';
    } else {
      label = 'ON';
      cssClass = '.pf-sidebar__dbg-info-square--green';
      title += 'Serving from cache. Ready for offline use';
    }

    const toggle = async () => {
      if (ctl.bypassed) {
        ctl.setBypass(false);
        return;
      }
      showModal({
        title: 'Disable service worker?',
        content: m(
          'div',
          m(
            'p',
            `If you continue the service worker will be disabled until
                      manually re-enabled.`,
          ),
          m(
            'p',
            `All future requests will be served from the network and the
                    UI won't be available offline.`,
          ),
          m(
            'p',
            `You should do this only if you are debugging the UI
                    or if you are experiencing caching-related problems.`,
          ),
          m(
            'p',
            `Disabling will cause a refresh of the UI, the current state
                    will be lost.`,
          ),
        ),
        buttons: [
          {
            text: 'Disable and reload',
            primary: true,
            action: () => ctl.setBypass(true).then(() => location.reload()),
          },
          {text: 'Cancel'},
        ],
      });
    };

    return m(
      `.pf-sidebar__dbg-info-square${cssClass}`,
      {title, ondblclick: toggle},
      m('div', 'SW'),
      m('div', label),
    );
  },
};

class SidebarFooter implements m.ClassComponent<OptionalTraceImplAttrs> {
  view({attrs}: m.CVnode<OptionalTraceImplAttrs>) {
    return m(
      '.pf-sidebar__footer',
      m(EngineRPCWidget, attrs),
      m(ServiceWorkerWidget),
      m(
        '.pf-sidebar__version',
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
}

class HiringBanner implements m.ClassComponent {
  view() {
    return m(
      '.pf-hiring-banner',
      m(
        'a',
        {
          href: 'http://go/perfetto-open-roles',
          target: '_blank',
        },
        "We're hiring!",
      ),
    );
  }
}

export class Sidebar implements m.ClassComponent {
  private _redrawWhileAnimating = new Animation(() => raf.scheduleFullRedraw());
  private _asyncJobPending = new Set<string>();
  private _sectionExpanded = new Map<string, boolean>();

  view({attrs}: m.CVnode) {
    const app = AppImpl.instance;
    const sidebar = app.sidebar;
    const trace = app.trace;
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
      shouldShowHiringBanner() && m(HiringBanner),
      this.renderSidebarHeader(app),
      this.renderSidebarContent(trace),
      m(SidebarFooter, attrs),
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
      m(Button, {
        icon: 'menu',
        className: 'pf-sidebar-button',
        onclick: () => sidebar.toggleVisibility(),
      }),
    );
  }

  private renderSidebarContent(trace: TraceImpl | undefined) {
    return m(
      '.pf-sidebar__content',
      (Object.keys(SIDEBAR_SECTIONS) as SidebarSections[]).map((sectionId) =>
        this.renderSection(sectionId, trace),
      ),
    );
  }

  private renderSection(
    sectionId: SidebarSections,
    trace: TraceImpl | undefined,
  ) {
    const section = SIDEBAR_SECTIONS[sectionId];

    // Combine plugin-registered items with reactive built-in items
    const allItems: SidebarMenuItemInternal[] = [
      ...AppImpl.instance.sidebar.menuItems
        .valuesAsArray()
        .filter((item) => item.section === sectionId),
    ];

    // Add section-specific global and trace items, and determine default collapsed state
    let defaultCollapsed = false;
    switch (sectionId) {
      case 'current_trace':
        if (trace !== undefined) {
          allItems.push(...getCurrentTraceItems(trace));
        }
        break;
      case 'convert_trace':
        if (trace !== undefined) {
          allItems.push(...getConvertTraceItems(trace));
        }
        defaultCollapsed = true;
        break;
      case 'support':
        allItems.push(...getSupportGlobalItems());
        if (trace !== undefined) {
          allItems.push(...getSupportTraceItems(trace));
        }
        defaultCollapsed = true;
        break;
    }

    const menuItems = allItems
      .sort((a, b) => (a.sortOrder ?? 0) - (b.sortOrder ?? 0))
      .map((item) => this.renderItem(item));

    // Don't render empty sections.
    if (menuItems.length === 0) return undefined;

    const expanded = getOrCreate(
      this._sectionExpanded,
      sectionId,
      () => !defaultCollapsed,
    );
    return m(
      `section.pf-sidebar__section`,
      {
        className: classNames(expanded && 'pf-sidebar__section--expanded'),
      },
      m(
        '.pf-sidebar__section-header',
        {
          onclick: () => {
            this._sectionExpanded.set(sectionId, !expanded);
          },
        },
        m('h1', {title: section.title}, section.title),
        m('h2', section.summary),
      ),
      m(
        '.pf-sidebar__section-content',
        sectionId === 'current_trace' &&
          trace?.traceInfo.traceTitle &&
          m('span.pf-sidebar__trace-file-name', trace.traceInfo.traceTitle),
        m('ul', menuItems),
      ),
    );
  }

  private renderItem(item: SidebarMenuItemInternal): m.Child {
    let href = '#';
    let disabled = false;
    let target = null;
    let isActive = false;
    let command: Command | undefined = undefined;
    let tooltip = valueOrCallback(item.tooltip);
    let onclick: (() => unknown | Promise<unknown>) | undefined = undefined;
    const commandId = 'commandId' in item ? item.commandId : undefined;
    const action = 'action' in item ? item.action : undefined;
    let text = valueOrCallback(item.text);
    const disabReason: boolean | string | undefined = valueOrCallback(
      item.disabled,
    );

    if (disabReason === true || typeof disabReason === 'string') {
      disabled = true;
      onclick = () => typeof disabReason === 'string' && alert(disabReason);
    } else if (action !== undefined) {
      onclick = action;
    } else if (commandId !== undefined) {
      const cmdMgr = AppImpl.instance.commands;
      command = cmdMgr.hasCommand(commandId ?? '')
        ? cmdMgr.getCommand(commandId)
        : undefined;
      if (command === undefined) {
        disabled = true;
      } else {
        text = text !== undefined ? text : command.name;
        if (command.defaultHotkey !== undefined) {
          tooltip =
            `${tooltip ?? command.name}` +
            ` [${formatHotkey(command.defaultHotkey)}]`;
        }
        onclick = () => cmdMgr.runCommand(commandId);
      }
    }

    if ('href' in item && item.href !== undefined) {
      href = item.href;
      target = href.startsWith('#') ? null : '_blank';
      isActive = pageMatchesHref(href);
    }

    const isLink = href !== '#';
    const iconEl =
      exists(item.icon) &&
      m(Icon, {
        className: 'pf-sidebar__button-icon',
        icon: valueOrCallback(item.icon),
      });
    const spinnerEl =
      this._asyncJobPending.has(item.id) &&
      m(Spinner, {className: 'pf-sidebar__spinner'});
    const cssClass = valueOrCallback(item.cssClass);

    return m(
      'li.pf-sidebar__item',
      {
        key: item.id, // This is to work around a mithril bug (b/449784590).
        className: classNames(isActive && 'pf-active'),
      },
      isLink
        ? m(
            'a',
            {
              className: cssClass,
              onclick: onclick && this.wrapClickHandler(item.id, onclick),
              href,
              target,
              disabled,
              title: tooltip,
            },
            iconEl,
            text,
            target === '_blank' &&
              m(Icon, {
                className: 'pf-sidebar__external-link-icon',
                icon: Icons.ExternalLink,
              }),
            spinnerEl,
          )
        : m(
            'button',
            {
              className: cssClass,
              onclick: onclick && this.wrapClickHandler(item.id, onclick),
              disabled,
              title: tooltip,
            },
            iconEl,
            text,
            spinnerEl,
          ),
    );
  }

  // Creates the onClick handlers for the items which provided a function in the
  // `action` member. The function can be either sync or async.
  // What we want to achieve here is the following:
  // - If the action is async (returns a Promise), we want to render a spinner,
  //   next to the menu item, until the promise is resolved.
  // - [Minor] we want to call e.preventDefault() to override the behaviour of
  //   the <a href='#'> which gets rendered for accessibility reasons.
  private wrapClickHandler(itemId: string, itemAction: Function) {
    return (e: Event) => {
      e.preventDefault(); // Make the <a href="#"> a no-op.
      const res = itemAction();
      if (!(res instanceof Promise)) return;
      if (this._asyncJobPending.has(itemId)) {
        return; // Don't queue up another action if not yet finished.
      }
      this._asyncJobPending.add(itemId);
      res.finally(() => {
        this._asyncJobPending.delete(itemId);
        raf.scheduleFullRedraw();
      });
    };
  }
}

export function pageMatchesHref(href: string): boolean {
  if (!href.startsWith('#!')) return false;
  const currentHash = window.location.hash;
  if (currentHash.length > 0 && !currentHash.startsWith('#!')) return false;
  const currentPage = Router.getCurrentRoute().page;
  const hrefPage = Router.parseFragment(href).page;
  return hrefPage === currentPage;
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

  items.push({
    id: 'perfetto.Timeline',
    section: 'current_trace',
    sortOrder: 10,
    text: 'Timeline',
    href: '#!/viewer',
    icon: 'line_style',
  });

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
    id: 'perfetto.LegacyUI',
    section: 'convert_trace',
    text: 'Switch to legacy UI',
    action: async () => await openCurrentTraceWithOldUI(trace),
    icon: 'filter_none',
    disabled: downloadDisabled,
  });

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
function getSupportGlobalItems(): SidebarMenuItemInternal[] {
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
      href: getBugReportUrl(),
      icon: 'bug_report',
    },
  ];
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

// Used to deal with fields like the entry name, which can be either a direct
// string or a callback that returns the string.
function valueOrCallback<T>(value: T | (() => T)): T;
function valueOrCallback<T>(value: T | (() => T) | undefined): T | undefined;
function valueOrCallback<T>(value: T | (() => T) | undefined): T | undefined {
  if (value === undefined) return undefined;
  return value instanceof Function ? value() : value;
}
