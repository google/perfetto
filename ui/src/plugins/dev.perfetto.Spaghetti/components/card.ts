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
import {Icon} from '../../../widgets/icon';
import './card.scss';

export interface CardAttrs {
  readonly className?: string;
  readonly dataHash?: string;
  readonly icon?: string;
  readonly hue?: number;
  readonly title: string;
  readonly content: string;
  readonly meta?: string;
  readonly subheader?: m.Children;
  readonly badges?: m.Children;
}

export function Card(): m.Component<CardAttrs> {
  return {
    view({
      attrs: {
        className,
        dataHash,
        icon,
        hue,
        title,
        content,
        meta,
        subheader,
        badges,
      },
    }) {
      return m(
        '.pf-spag-card',
        {
          'className': classNames(className),
          'data-card-hash': dataHash,
          'style': hue !== undefined ? {'--pf-card-hue': `${hue}`} : undefined,
        },
        [
          m('.pf-spag-card-header', [
            icon !== undefined &&
              icon !== '' &&
              m(Icon, {icon, className: 'pf-spag-card-icon'}),
            m('span.pf-spag-card-title', title),
            Boolean(meta) && m('span.pf-spag-card-meta', meta),
            Boolean(badges) && m('.pf-spag-card-badges', badges),
          ]),
          Boolean(subheader) && m('.pf-spag-card-subheader', subheader),
          m('pre.pf-spag-card-body', content),
        ],
      );
    },
  };
}

export namespace Card {
  export const Badge: m.Component<{
    readonly label: string;
    readonly variant?: 'hit' | 'miss' | 'hits' | 'live' | 'orphan';
  }> = {
    view({attrs}) {
      const variantClass = attrs.variant
        ? `pf-spag-card-badge--${attrs.variant}`
        : '';
      return m(
        'span.pf-spag-card-badge',
        {className: variantClass},
        attrs.label,
      );
    },
  };

  export const Time: m.Component<{readonly label: string}> = {
    view({attrs}) {
      return m('span.pf-spag-card-time', attrs.label);
    },
  };
}
