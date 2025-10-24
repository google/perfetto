// Copyright (C) 2023 The Android Open Source Project
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

interface DetailsShellAttrs {
  // Additional class names to apply to the shell
  readonly className?: string;
  // Main title of the details shell.
  readonly title: m.Children;
  // A little descriptive text to be placed next to the title in a smaller font.
  readonly description?: m.Children;
  // Buttons to be rendered on the right side of the header bar.
  readonly buttons?: m.Children;
  // Vertically fill parent container and disable scrolling
  readonly fillHeight?: boolean;
}

// A shell for details panels to be more visually consistent.
// It provides regular placement for the header bar and placement of buttons
export class DetailsShell implements m.ClassComponent<DetailsShellAttrs> {
  view({attrs, children}: m.Vnode<DetailsShellAttrs>) {
    const {title, description, buttons, fillHeight = true, className} = attrs;

    return m(
      'section.pf-details-shell',
      {class: classNames(fillHeight && 'pf-fill-height', className)},
      m(
        'header.pf-header-bar',
        m('h1.pf-header-title', title),
        m('span.pf-header-description', description),
        m('nav.pf-header-buttons', buttons),
      ),
      m('article.pf-content', children),
    );
  }
}
