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

import {Time, type time} from '../../base/time';

export interface DumpRouteRef {
  readonly upid: number;
  readonly ts: time;
}

export type NavLinkTab =
  | {tab: 'overview'}
  | {tab: 'flamegraph'}
  | {tab: 'classes'; rootClass?: string}
  | {tab: 'objects'; cls?: string}
  | {tab: 'dominators'}
  | {tab: 'bitmaps'; id?: number; filterKey?: string}
  | {tab: 'strings'; q?: string}
  | {tab: 'arrays'; arrayHash?: string}
  | {tab: 'callstack'}
  | {tab: 'object'; id: number; label?: string}
  | {
      tab: 'flamegraph-objects';
      pathHashes?: string;
      isDominator?: boolean;
    };

export type NavLink = NavLinkTab & {
  dump?: DumpRouteRef;
};

export type NavTabName = NavLinkTab['tab'];
export type DefaultNavTab = 'overview' | 'flamegraph';

const HEAPDUMP_ROUTE_PREFIX = '#!/heapdump';

function queryParam(key: string, value?: string): string {
  return value ? `${key}=${encodeURIComponent(value)}` : '';
}

function pathSegment(base: string, value?: string): string {
  return value ? `${base}_${encodeURIComponent(value)}` : base;
}

function navTabToSubpage(nav: NavLinkTab): string {
  switch (nav.tab) {
    case 'overview':
      return 'overview';
    case 'flamegraph':
      return 'flamegraph';
    case 'classes': {
      const q = queryParam('root', nav.rootClass);
      return q ? `classes?${q}` : 'classes';
    }
    case 'objects':
      return pathSegment('objects', nav.cls);
    case 'dominators':
      return 'dominators';
    case 'bitmaps': {
      const path = pathSegment(
        'bitmaps',
        nav.id !== undefined ? `0x${nav.id.toString(16)}` : undefined,
      );
      const q = queryParam('fk', nav.filterKey);
      return q ? `${path}?${q}` : path;
    }
    case 'strings': {
      const q = queryParam('q', nav.q);
      return q ? `strings?${q}` : 'strings';
    }
    case 'arrays': {
      const q = queryParam('ah', nav.arrayHash);
      return q ? `arrays?${q}` : 'arrays';
    }
    case 'callstack':
      return 'callstack';
    case 'object':
      return pathSegment('object', `0x${nav.id.toString(16)}`);
    case 'flamegraph-objects': {
      if (nav.pathHashes === undefined) {
        return 'flamegraph_objects';
      }
      const flag = nav.isDominator ? '1' : '0';
      return `flamegraph_objects_${flag}_${encodeURIComponent(nav.pathHashes)}`;
    }
  }
}

/**
 * Generates the subpage portion (after `#!/heapdump/`) for a `NavLink`.
 * E.g., `42-1000/objects_java.lang.String` or `objects_java.lang.String`.
 */
export function generateNavSubpage(nav: NavLink): string {
  const tabSubpage = navTabToSubpage(nav);
  if (nav.dump !== undefined) {
    return `${nav.dump.upid}-${nav.dump.ts}/${tabSubpage}`;
  }
  return tabSubpage;
}

/**
 * Generates a full href string suitable for `<a href={...}>` or `trace.navigate(...)`.
 * E.g., `#!/heapdump/42-1000/objects_java.lang.String`.
 */
export function generateNavLink(nav: NavLink): string {
  const subpage = generateNavSubpage(nav);
  return `${HEAPDUMP_ROUTE_PREFIX}/${subpage}`;
}

function stripRoutePrefix(hrefOrSubpage: string): string {
  let s = hrefOrSubpage.trim();
  if (s.startsWith('#!/heapdump/')) {
    s = s.slice('#!/heapdump/'.length);
  } else if (s === '#!/heapdump') {
    s = '';
  } else if (s.startsWith('/heapdump/')) {
    s = s.slice('/heapdump/'.length);
  } else if (s === '/heapdump') {
    s = '';
  }
  if (s.startsWith('/')) {
    s = s.slice(1);
  }
  return s;
}

function parseTabSubpage(
  tabSubpage: string,
  defaultTab: DefaultNavTab,
): NavLinkTab {
  if (!tabSubpage) {
    return {tab: defaultTab};
  }

  const [path, queryStr] = tabSubpage.split('?', 2);
  const sp = new URLSearchParams(queryStr ?? '');

  let view: string;
  let param: string;
  if (path.startsWith('flamegraph_objects')) {
    view = 'flamegraph-objects';
    param = path.slice('flamegraph_objects'.length + 1) || '';
  } else {
    const idx = path.indexOf('_');
    view = idx === -1 ? path : path.slice(0, idx);
    param = idx === -1 ? '' : path.slice(idx + 1);
  }

  switch (view) {
    case '':
      return {tab: defaultTab};
    case 'overview':
      return {tab: 'overview'};
    case 'flamegraph':
      return {tab: 'flamegraph'};
    case 'classes': {
      const rootClass = sp.get('root') ?? undefined;
      return rootClass !== undefined
        ? {tab: 'classes', rootClass}
        : {tab: 'classes'};
    }
    case 'objects': {
      const cls = param ? decodeURIComponent(param) : undefined;
      return cls !== undefined ? {tab: 'objects', cls} : {tab: 'objects'};
    }
    case 'dominators':
      return {tab: 'dominators'};
    case 'bitmaps': {
      const selectedId = param.startsWith('0x')
        ? parseInt(param.slice(2), 16)
        : param
          ? parseInt(param, 10)
          : 0;
      const filterKey = sp.get('fk') ?? undefined;
      const res: {tab: 'bitmaps'; id?: number; filterKey?: string} = {
        tab: 'bitmaps',
      };
      if (selectedId) res.id = selectedId;
      if (filterKey) res.filterKey = filterKey;
      return res;
    }
    case 'strings': {
      const q = sp.get('q') ?? undefined;
      return q ? {tab: 'strings', q} : {tab: 'strings'};
    }
    case 'arrays': {
      const arrayHash = sp.get('ah') ?? undefined;
      return arrayHash ? {tab: 'arrays', arrayHash} : {tab: 'arrays'};
    }
    case 'callstack':
      return {tab: 'callstack'};
    case 'object': {
      const raw = param || '0';
      const id = raw.startsWith('0x')
        ? parseInt(raw.slice(2), 16)
        : parseInt(raw, 10);
      return {tab: 'object', id: id || 0};
    }
    case 'flamegraph-objects': {
      if (param === '') {
        return {tab: 'flamegraph-objects'};
      }
      const us = param.indexOf('_');
      const isDominator = us > 0 && param.slice(0, us) === '1';
      const rest = us === -1 ? param : param.slice(us + 1);
      return {
        tab: 'flamegraph-objects',
        pathHashes: decodeURIComponent(rest),
        isDominator,
      };
    }
    default:
      return {tab: 'overview'};
  }
}

/**
 * Parses a full href (e.g. `#!/heapdump/42-1000/objects_java.lang.String`)
 * or a subpage string (e.g. `42-1000/objects_java.lang.String`) into a structured `NavLink`.
 */
export function parseNavLink(
  hrefOrSubpage: string | undefined,
  defaultTab: DefaultNavTab = 'overview',
): NavLink {
  if (!hrefOrSubpage) {
    return {tab: defaultTab};
  }

  const subpage = stripRoutePrefix(hrefOrSubpage);
  if (!subpage) {
    return {tab: defaultTab};
  }

  const match = subpage.match(/^(\d+)-(\d+)(?:\/(.*))?$/);
  if (match) {
    const upid = Number(match[1]);
    const ts = Time.fromRaw(BigInt(match[2]));
    const tabSubpage = match[3] ?? '';
    return {
      ...parseTabSubpage(tabSubpage, defaultTab),
      dump: {upid, ts},
    };
  }

  return parseTabSubpage(subpage, defaultTab);
}
