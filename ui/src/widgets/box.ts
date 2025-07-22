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
import {classForSpacing, HTMLAttrs, Spacing} from './common';

export interface BoxAttrs extends HTMLAttrs {
  readonly fillHeight?: boolean;
  readonly spacing?: Spacing;
}

export class Box implements m.ClassComponent<BoxAttrs> {
  view({attrs, children}: m.CVnode<BoxAttrs>) {
    const {fillHeight = false, className, spacing = 'medium', ...rest} = attrs;
    return m(
      '.pf-box',
      {
        ...rest,
        className: classNames(
          className,
          fillHeight && 'pf-box--fill-height',
          classForSpacing(spacing),
        ),
      },
      children,
    );
  }
}
