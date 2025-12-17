// Copyright (C) 2025 The Android Open Source Project
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
import {classForIntent, HTMLAttrs, Intent} from './common';
import {assertExists} from '../base/logging';
import {Card} from './card';
import {classNames} from '../base/classnames';
import {Anchor} from './anchor';
import {childrenValid} from '../base/mithril_utils';

export interface SettingsShellAttrs extends HTMLAttrs {
  readonly title: string;
  readonly stickyHeaderContent?: m.Children;
}

export class SettingsShell implements m.ClassComponent<SettingsShellAttrs> {
  private observer?: IntersectionObserver;

  view(vnode: m.Vnode<SettingsShellAttrs>) {
    const {
      title,
      stickyHeaderContent: headerContent,
      ...htmlAttrs
    } = vnode.attrs;
    return m(
      '.pf-settings-shell',
      htmlAttrs,
      m(
        '.pf-settings-shell__title',
        m('.pf-settings-shell__centred', m('h1', title)),
      ),
      m(
        '.pf-settings-shell__header',
        m('.pf-settings-shell__centred', headerContent),
      ),
      m(
        '.pf-settings-shell__content',
        m('.pf-settings-shell__centred', vnode.children),
      ),
    );
  }

  oncreate(vnode: m.VnodeDOM<SettingsShellAttrs, this>) {
    const canary = assertExists(
      vnode.dom.querySelector('.pf-settings-shell__title'),
    );
    const header = assertExists(
      vnode.dom.querySelector('.pf-settings-shell__header'),
    );

    this.observer = new IntersectionObserver(
      ([entry]) => {
        header.classList.toggle(
          'pf-settings-shell__header--stuck',
          !entry.isIntersecting,
        );
      },
      {threshold: [0]},
    );

    this.observer.observe(canary);
  }

  onremove() {
    this.observer?.disconnect();
  }
}

export interface SettingsCardAttrs extends HTMLAttrs {
  readonly id?: string;
  readonly title: string;
  readonly controls: m.Children;
  readonly focused?: boolean;
  readonly description?: m.Children;
  readonly accent?: Intent;
  readonly linkHref?: string;
}

export class SettingsCard implements m.ClassComponent<SettingsCardAttrs> {
  oncreate(vnode: m.VnodeDOM<SettingsCardAttrs>) {
    if (vnode.attrs.focused) {
      vnode.dom.scrollIntoView({behavior: 'smooth', block: 'center'});
    }
  }

  view(vnode: m.Vnode<SettingsCardAttrs>) {
    const {
      className,
      id,
      title,
      controls,
      focused,
      description,
      accent,
      linkHref,
      ...rest
    } = vnode.attrs;
    return m(
      Card,
      {
        ...rest,
        id,
        className: classNames(
          'pf-settings-card',
          accent !== undefined && classForIntent(accent),
          focused && 'pf-settings-card--focused',
          className,
        ),
      },
      [
        m(
          '.pf-settings-card__details',
          m(
            '.pf-settings-card__title',
            title,
            m(Anchor, {
              href: linkHref,
              className: 'pf-settings-card__link',
              icon: 'link',
              title: 'Link to this flag',
            }),
          ),
          id && m('.pf-settings-card__id', id),
          childrenValid(description) &&
            m('.pf-settings-card__description', description),
        ),
        controls,
      ],
    );
  }
}
