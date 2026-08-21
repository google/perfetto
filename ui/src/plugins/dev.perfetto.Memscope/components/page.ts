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
import './page.scss';

// Page — the outermost shell for a Memscope view. It owns the scroll container,
// the light/dark theme variables, and a centred, max-width content column, so
// every Memscope page shares the same chrome and "raised cards on a cool-grey
// canvas" look. Wrap a view's whole content in it and drop a Page.Title (and
// optional Page.Subtitle) at the top. SubPage is a lighter wrapper for a nested
// section that just needs the standard vertical gap + entrance animation
// without re-establishing the full page chrome.

export function Page(): m.Component {
  return {
    view({children}: m.Vnode) {
      return m('.pf-memscope-page', m('.pf-memscope-page__content', children));
    },
  };
}

export namespace Page {
  export const Title: m.Component = {
    view({children}: m.Vnode) {
      return m('.pf-memscope-page__title', children);
    },
  };

  export const Subtitle: m.Component = {
    view({children}: m.Vnode) {
      return m('.pf-memscope-page__subtitle', children);
    },
  };
}

export function SubPage(): m.Component {
  return {
    view({children}: m.Vnode) {
      return m('.pf-memscope-subpage', children);
    },
  };
}
