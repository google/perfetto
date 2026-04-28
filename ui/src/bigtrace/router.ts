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

// Simple hash-based router for BigTrace.
// Uses #! prefix (e.g., #!/query, #!/settings) to match the existing
// sidebar link format.
//
// This replaces Mithril's m.route() so that all rendering goes through
// the raf scheduler's mount system. m.route() bypasses the raf scheduler
// because it caches a reference to the original m.mount, which breaks
// cross-tree redraws (e.g. portal-based popups like the omnibox dropdown).

export function getCurrentRoute(): string {
  const hash = window.location.hash;
  if (hash.startsWith('#!')) {
    const route = hash.slice(2);
    return route || '/';
  }
  return '/';
}

export function setRoute(route: string): void {
  window.location.hash = '!' + route;
}

export function initRouter(): void {
  window.addEventListener('hashchange', () => {
    m.redraw();
  });
}
