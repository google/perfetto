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

export interface EmptyStateAttrs {
  // Which material icon to show.
  // Defaults to 'search'.
  icon?: string;

  // Some text to show under the icon. No text shown if omitted.
  title?: string;

  // Additional class name applied to our container.
  className?: string;
}

// Something to show when there's nothing else to show!
// Features a large icon, followed by some text explaining what went wrong, and
// some optional content passed as children elements, usually containing common
// actions for things you might want to do next (e.g. clear a search box).
export class EmptyState implements m.ClassComponent<EmptyStateAttrs> {
  view({attrs, children}: m.Vnode<EmptyStateAttrs, this>): void | m.Children {
    const {
      icon = 'search', // Icon defaults to the search symbol
      title,
      className,
    } = attrs;
    return m(
      '.pf-empty-state',
      {className},
      m('i.material-icons', icon),
      title && m('.pf-empty-state-title', title),
      m('.pf-empty-state-content', children),
    );
  }
}
