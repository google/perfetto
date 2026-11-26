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

import './card.scss';
import m from 'mithril';
import {classForDensity, type Density, type HTMLAttrs} from './common';

import {classNames} from '../base/classnames';

export interface CardAttrs extends HTMLAttrs {
  // Whether the card should have a hover effect.
  readonly interactive?: boolean;
  readonly density?: Density;
}

export class Card implements m.ClassComponent<CardAttrs> {
  view(vnode: m.Vnode<CardAttrs>): m.Children {
    const {interactive, density, className, ...attrs} = vnode.attrs;
    return m(
      '.pf-card',
      {
        ...attrs,
        className: classNames(
          className,
          interactive && 'pf-interactive',
          density !== undefined && classForDensity(density),
        ),
      },
      vnode.children,
    );
  }
}

export interface CardStackAttrs extends HTMLAttrs {
  // The direction of the stack, defaults to 'vertical'.
  readonly direction?: 'vertical' | 'horizontal';
  readonly density?: Density;
}

export class CardStack implements m.ClassComponent<CardStackAttrs> {
  view({attrs, children}: m.Vnode<CardStackAttrs>): m.Children {
    const {direction = 'vertical', density, className, ...htmlAttrs} = attrs;
    return m(
      '.pf-card-stack',
      {
        ...htmlAttrs,
        className: classNames(
          direction === 'horizontal' && 'pf-card-stack--horizontal',
          density !== undefined && classForDensity(density),
          className,
        ),
      },
      children,
    );
  }
}
