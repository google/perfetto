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

// Memscope is focused on one process, named in the route as
// `#!/memscope?process=<name>` or `#!/memory?process=<name>`. Names, not pids,
// identify a process across tools and restarts.

import type {RouteArgs} from '../../public/route_schema';

const MEMSCOPE_LIVE_ROUTE = '/memscope';

// The standalone Memscope product, used when linking from another origin.
const MEMSCOPE_PRODUCT_URL = 'https://memscope.perfetto.dev/';

export function processRouteArg(args: RouteArgs): string | undefined {
  const v = args['process'];
  if (typeof v !== 'string') return undefined;
  const name = v.trim();
  return name === '' ? undefined : name;
}

export function memscopeLiveHref(): string {
  return `#!${MEMSCOPE_LIVE_ROUTE}`;
}

export function memscopeProcessHref(name: string): string {
  return `#!${MEMSCOPE_LIVE_ROUTE}?process=${encodeURIComponent(name)}`;
}

export function memscopeProductProcessUrl(name: string): string {
  return `${MEMSCOPE_PRODUCT_URL}${memscopeProcessHref(name)}`;
}

// Items whose name matches `name`: exact matches if any, otherwise
// case-insensitive ones, otherwise those containing it.
export function matchProcessesByName<T>(
  items: ReadonlyArray<T>,
  name: string,
  nameOf: (item: T) => string,
): T[] {
  const exact = items.filter((i) => nameOf(i) === name);
  if (exact.length > 0) return exact;
  const lower = name.toLowerCase();
  const ci = items.filter((i) => nameOf(i).toLowerCase() === lower);
  if (ci.length > 0) return ci;
  return items.filter((i) => nameOf(i).toLowerCase().includes(lower));
}
