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
import {classNames} from '../../base/classnames';
import {formatHotkey} from '../../base/hotkeys';
import {Icons} from '../../base/semantic_icons';
import {exists} from '../../base/utils';
import type {AppImpl} from '../../core/app_impl';
import {raf} from '../../core/raf_scheduler';
import {Router} from '../../core/router';
import type {SidebarMenuItemInternal} from '../../core/sidebar_manager';
import type {Command} from '../../public/commands';
import {Icon} from '../../widgets/icon';
import {Spinner} from '../../widgets/spinner';

export interface SidebarItemAttrs {
  readonly app: AppImpl;
  readonly item: SidebarMenuItemInternal;
}

export class SidebarItem implements m.ClassComponent<SidebarItemAttrs> {
  private pending = false;

  view({attrs}: m.CVnode<SidebarItemAttrs>): m.Children {
    const item = attrs.item;
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
      const app = attrs.app;
      const cmdMgr = (app.trace ?? app).commands;
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
      this.pending && m(Spinner, {className: 'pf-sidebar__spinner'});
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
              onclick: onclick && this.wrapClickHandler(onclick),
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
              onclick: onclick && this.wrapClickHandler(onclick),
              disabled,
              title: tooltip,
            },
            iconEl,
            text,
            spinnerEl,
          ),
    );
  }

  // Wraps the onClick for items that provided an `action`. The action can be
  // sync or async; for async actions we render a spinner until it resolves and
  // also debounce re-clicks. We always preventDefault to neutralise the
  // accessibility-driven `<a href="#">`.
  private wrapClickHandler(itemAction: Function) {
    return (e: Event) => {
      e.preventDefault();
      const res = itemAction();
      if (!(res instanceof Promise)) return;
      if (this.pending) return; // Don't queue another action while one runs.
      this.pending = true;
      res.finally(() => {
        this.pending = false;
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

// Used to deal with fields like the entry name, which can be either a direct
// string or a callback that returns the string.
function valueOrCallback<T>(value: T | (() => T)): T;
function valueOrCallback<T>(value: T | (() => T) | undefined): T | undefined;
function valueOrCallback<T>(value: T | (() => T) | undefined): T | undefined {
  if (value === undefined) return undefined;
  return value instanceof Function ? value() : value;
}
