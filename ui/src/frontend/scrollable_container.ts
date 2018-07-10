// Copyright (C) 2018 The Android Open Source Project
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

import * as m from 'mithril';

export const ScrollableContainer = {
  view({attrs, children}) {
    return m(
        '.scrollableContainer',
        {
          style: {
            width: attrs.width.toString() + 'px',
            height: attrs.height.toString() + 'px',
            'overflow-y': 'auto',
            'overflow-x': 'hidden',
            'will-change': 'transform',
            position: 'relative'
          }
        },
        m(ScrollableContent, {contentHeight: attrs.contentHeight}, children));
  },

  oncreate({dom, attrs}) {
    dom.addEventListener('scroll', () => {
      attrs.onPassiveScroll(dom.scrollTop);
    }, {passive: true});
  }
} as m.Component<{
  width: number,
  height: number,
  contentHeight: number,
  onPassiveScroll: (scrollTop: number) => void,
}>;


const ScrollableContent = {
  view({attrs, children}) {
    return m(
        '.scrollableContent',
        {
          style: {
            height: attrs.contentHeight.toString() + 'px',
            overflow: 'hidden',
            position: 'relative'
          }
        },
        children);
  }
} as m.Component<{contentHeight: number}>;
