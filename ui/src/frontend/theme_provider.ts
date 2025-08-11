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

interface ThemeProviderAttrs {
  readonly theme: 'dark' | 'light';
}

export class ThemeProvider implements m.Component<ThemeProviderAttrs> {
  view(vnode: m.Vnode<ThemeProviderAttrs>) {
    // This component is used to provide the theme context to the children.
    // It does not render anything itself.
    return m(
      '.pf-theme-provider',
      {className: `pf-theme-provider--${vnode.attrs.theme}`},
      vnode.children,
    );
  }
}
