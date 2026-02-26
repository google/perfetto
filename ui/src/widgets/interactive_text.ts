// Copyright (C) 2024 The Android Open Source Project
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
import {HTMLAnchorAttrs} from './common';
import {Icon} from './icon';

export interface InteractiveTextAttrs extends HTMLAnchorAttrs {
  // Optional icon to show at the end of the content.
  readonly icon?: string;

  // Optional icon to show at the start of the content.
  readonly startIcon?: string;
}

/**
 * A clickable widget that looks like regular text (no blue color or underline)
 * but shows a gray background on hover.
 */
export class InteractiveText implements m.ClassComponent<InteractiveTextAttrs> {
  view({attrs, children}: m.CVnode<InteractiveTextAttrs>) {
    const {icon, startIcon, ...htmlAttrs} = attrs;

    return m(
      'a.pf-interactive-text',
      htmlAttrs,
      startIcon &&
        m(Icon, {
          icon: startIcon,
          className: 'pf-interactive-text__icon--start',
        }),
      children,
      icon && m(Icon, {icon, className: 'pf-interactive-text__icon--end'}),
    );
  }
}
