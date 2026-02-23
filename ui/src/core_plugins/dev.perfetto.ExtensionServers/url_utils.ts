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

import {UserInput} from './types';

// Returns true if two servers point to the same location (ignoring auth,
// enabledModules, and enabled state).
export function sameServerLocation(a: UserInput, b: UserInput): boolean {
  if (a.type !== b.type) return false;
  if (a.type === 'github' && b.type === 'github') {
    return a.repo === b.repo && a.ref === b.ref && a.path === b.path;
  }
  if (a.type === 'https' && b.type === 'https') {
    return a.url === b.url;
  }
  return false;
}

// Converts a server location into a display-friendly string.
export function makeDisplayUrl(server: UserInput): string {
  if (server.type === 'github') {
    const path = server.path !== '/' ? `:${server.path}` : '';
    return `${server.repo}${path} @ ${server.ref}`;
  }
  return server.url.replace(/^https?:\/\//, '');
}

// Joins a base path (e.g. "/" or "/subdir") with a resource path, stripping
// redundant slashes.
export function joinPath(base: string, resource: string): string {
  const trimmed = base.replace(/^\/+|\/+$/g, '');
  return trimmed ? `${trimmed}/${resource}` : resource;
}

// Auto-add https:// if no protocol specified.
export function normalizeHttpsUrl(input: string): string {
  let url = input.trim();
  if (!url.includes('://')) {
    url = `https://${url}`;
  }
  return url.replace(/\/+$/, '');
}
