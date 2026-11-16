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

// The DOM attribute that names the owner of an area of the screen. When labels
// are nested, the innermost one wins. Core writes it wherever it draws
// registered UI. Placing it here in base/ so that widgets, which can't import
// core/, can write it too.
export const OWNER_ATTRIBUTE = 'data-plugin';

/**
 * Wraps `children` in an element that names their owner. The element uses
 * display: contents, so it doesn't affect layout.
 */
export function withOwner(
  owner: string | undefined,
  children: m.Children,
): m.Vnode {
  return m(
    '',
    {style: {display: 'contents'}, [OWNER_ATTRIBUTE]: owner},
    children,
  );
}
