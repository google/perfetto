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
import {HTMLAttrs} from './common';
import {classNames} from '../base/classnames';

export interface CardAttrs extends HTMLAttrs {
  readonly borderless?: boolean;
}

export class Card implements m.ClassComponent<CardAttrs> {
  view(vnode: m.Vnode<CardAttrs>): m.Children {
    const {borderless, className, ...htmlAttrs} = vnode.attrs;
    return m(
      '.pf-card',
      {
        className: classNames(borderless && 'pf-card--borderless', className),
        ...htmlAttrs,
      },
      vnode.children,
    );
  }
}

export class CardList implements m.ClassComponent<HTMLAttrs> {
  view(vnode: m.Vnode<{}>): m.Children {
    return m('.pf-card-list', vnode.children);
  }
}
