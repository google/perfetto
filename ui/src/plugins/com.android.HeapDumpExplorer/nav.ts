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
import type {time} from '../../base/time';
import {Anchor} from '../../widgets/anchor';
import type {HTMLAnchorAttrs} from '../../widgets/common';

// Identifies a heap dump. Structurally compatible with queries.HeapDump.
export interface DumpRef {
  readonly upid: number;
  readonly ts: time;
}

export type StaticHdeLink =
  | {readonly view: 'overview'}
  | {readonly view: 'flamegraph'}
  | {readonly view: 'dominators'}
  | {readonly view: 'callstack'}
  | {readonly view: 'classes'; readonly rootClass?: string}
  | {readonly view: 'objects'; readonly cls?: string}
  | {readonly view: 'bitmaps'; readonly filterKey?: string}
  | {readonly view: 'strings'; readonly q?: string}
  | {readonly view: 'arrays'; readonly arrayHash?: string};

// Everything that can be linked to within a dump. Filterable views take their
// filter as an extra path segment. Object inspectors and flamegraph
// drill-downs are addressed by what they show, and appear as an ephemeral tab
// while their URL is showing.
export type HdeLink =
  | StaticHdeLink
  | {
      readonly view: 'flamegraph-objects';
      readonly pathHashes: string;
      readonly isDominator: boolean;
    }
  | {readonly view: 'object'; readonly id: number};

// The URL segment identifying a dump: `<upid>-<ts>`. Also used as the key for
// per-dump persisted state.
export function dumpKey(dump: DumpRef): string {
  return `${dump.upid}-${dump.ts}`;
}

// The one place HDE hrefs are built. Every link within the plugin (tabs,
// cross-view links, programmatic navigation) goes through here, so the URL
// format only lives in this function and the page's route table.
export function makeHref(dump: DumpRef, link?: HdeLink): string {
  if (!link) {
    return `#!/heapdump/${dumpKey(dump)}`;
  } else {
    return `#!/heapdump/${dumpKey(dump)}/${makeSubpage(link)}`;
  }
}

function makeSubpage(link: HdeLink): string {
  switch (link.view) {
    case 'object':
      return segments('object', `0x${link.id.toString(16)}`);
    case 'flamegraph-objects':
      return segments(
        link.isDominator ? 'dominator-objects' : 'flamegraph-objects',
        link.pathHashes
          .split(',')
          .map((h) => `0x${BigInt.asUintN(64, BigInt(h.trim())).toString(16)}`)
          .join(','),
      );
    case 'classes':
      return segments('classes', link.rootClass);
    case 'objects':
      return segments('objects', link.cls);
    case 'bitmaps':
      return segments('bitmaps', link.filterKey);
    case 'strings':
      return segments('strings', link.q);
    case 'arrays':
      return segments('arrays', link.arrayHash);
    default:
      return link.view;
  }
}

// `base`, plus `value` as a URI-encoded path segment when it is non-empty.
function segments(base: string, value?: string): string {
  return value ? `${base}/${encodeURIComponent(value)}` : base;
}

interface HdeAnchorAttrs extends Omit<HTMLAnchorAttrs, 'href'> {
  // The dump the link points into.
  readonly dump: DumpRef;
  // Where within the dump.
  readonly to: HdeLink;
}

// The one way views link within HDE. Today it renders a plain href; if the
// location ever moves off the URL, only this (and makeHref) need to change.
export class HdeAnchor implements m.ClassComponent<HdeAnchorAttrs> {
  view({attrs, children}: m.CVnode<HdeAnchorAttrs>) {
    const {dump, to, ...rest} = attrs;
    return m(Anchor, {...rest, href: makeHref(dump, to)}, children);
  }
}
