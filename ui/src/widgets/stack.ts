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
import {classNames} from '../base/classnames';
import {HTMLAttrs} from './common';

interface StackAttrs extends HTMLAttrs {
  readonly orientation?: 'horizontal' | 'vertical';
  readonly fillHeight?: boolean;
  readonly gap?: 'none' | 'normal';
}

export class Stack implements m.ClassComponent<StackAttrs> {
  view({attrs, children}: m.CVnode<StackAttrs>) {
    const {
      orientation = 'vertical',
      fillHeight = false,
      gap = 'normal',
      className,
      ...htmlAttrs
    } = attrs;
    return m(
      '.pf-stack',
      {
        className: classNames(
          orientation === 'horizontal' && 'pf-stack--horiz',
          fillHeight && 'pf-stack--fill-height',
          gap === 'none' && 'pf-stack--gap-none',
          className,
        ),
        ...htmlAttrs,
      },
      children,
    );
  }
}

export class StackAuto implements m.ClassComponent {
  view({children}: m.CVnode) {
    return m('.pf-stack-auto', children);
  }
}

export class StackFixed implements m.ClassComponent {
  view({children}: m.CVnode) {
    return m('.pf-stack-fixed', children);
  }
}
