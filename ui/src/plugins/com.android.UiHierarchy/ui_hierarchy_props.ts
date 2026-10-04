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

// Curated properties shown in the Properties pane of both levels: sections
// of rows, built without default or empty values, and rendered by one
// component.

import type {UiHierarchyNode} from './ui_hierarchy_data';

// Marks the animation leash WM wraps around a layer while animating it.
export const LEASH_MARKER = ' - animation-leash of ';

// What a link in a property value points at.
export type PropTarget =
  // A SurfaceFlinger layer.
  | {readonly kind: 'layer'; readonly layerId: number}
  // A WindowManager window, selected on the Screen level.
  | {readonly kind: 'window'; readonly nodeId: string}
  // A process: shows only its windows.
  | {readonly kind: 'process'; readonly key: string}
  // A shell transition: compares across it.
  | {readonly kind: 'transition'; readonly transitionId: number}
  // A SQL query, run in a query results tab.
  | {readonly kind: 'query'; readonly title: string; readonly sql: string};

export interface PropLink {
  readonly text: string;
  // The full value, if `text` is shortened.
  readonly title?: string;
  readonly target: PropTarget;
}

export type PropValue =
  | {
      readonly kind: 'text';
      readonly text: string;
      readonly title?: string;
      readonly mono?: boolean;
    }
  // Monospace, with a copy button.
  | {readonly kind: 'copy'; readonly text: string}
  | {readonly kind: 'links'; readonly links: ReadonlyArray<PropLink>}
  | {readonly kind: 'list'; readonly items: ReadonlyArray<string>}
  | {readonly kind: 'chip'; readonly text: string};

// How a row differs from the previous snapshot. A removed row keeps its
// last value.
export type PropDiff =
  | {readonly kind: 'added'}
  | {readonly kind: 'changed'; readonly previous: PropValue}
  | {readonly kind: 'removed'};

export interface PropRow {
  readonly label: string;
  readonly value: PropValue;
  readonly diff?: PropDiff;
  // Detail rows, shown when the row is expanded (collapsed at first).
  readonly children?: ReadonlyArray<PropRow>;
}

export interface PropSection {
  readonly title: string;
  readonly rows: ReadonlyArray<PropRow>;
  // A link next to the title, e.g. to query the section's data.
  readonly action?: PropLink;
}

// Collects rows, dropping the ones without a value.
export class PropRowsBuilder {
  private readonly rows: PropRow[] = [];

  text(
    label: string,
    text: string | undefined,
    opts: {readonly title?: string; readonly mono?: boolean} = {},
  ): this {
    if (text !== undefined && text !== '') {
      this.rows.push({label, value: {kind: 'text', text, ...opts}});
    }
    return this;
  }

  copy(label: string, text: string | undefined): this {
    if (text !== undefined && text !== '') {
      this.rows.push({label, value: {kind: 'copy', text}});
    }
    return this;
  }

  links(label: string, links: ReadonlyArray<PropLink>): this {
    if (links.length > 0) {
      this.rows.push({label, value: {kind: 'links', links}});
    }
    return this;
  }

  list(label: string, items: ReadonlyArray<string>): this {
    if (items.length > 0) {
      this.rows.push({label, value: {kind: 'list', items}});
    }
    return this;
  }

  chip(label: string, text: string | undefined): this {
    if (text !== undefined && text !== '') {
      this.rows.push({label, value: {kind: 'chip', text}});
    }
    return this;
  }

  row(row: PropRow): this {
    this.rows.push(row);
    return this;
  }

  build(): PropRow[] {
    return this.rows;
  }
}

// Integers as is, others with at most 2 decimals: 0.4, 1.25, 28.
export function formatNumber(v: number): string {
  return Number.isInteger(v) ? `${v}` : v.toFixed(2).replace(/\.?0+$/, '');
}

export interface Bounds {
  readonly left: number;
  readonly top: number;
  readonly right: number;
  readonly bottom: number;
}

// "[l, t, r, b]  w x h px".
export function formatBounds(b: Bounds): string {
  const f = formatNumber;
  return (
    `[${f(b.left)}, ${f(b.top)}, ${f(b.right)}, ${f(b.bottom)}]  ` +
    `${f(b.right - b.left)} x ${f(b.bottom - b.top)} px`
  );
}

// Shortens names for tree rows, canvas labels and links; the full name goes
// in the tooltip:
// - "com.example/com.example.ui.MainActivity" -> "MainActivity"
// - "com.example/.MainActivity" -> "MainActivity"
// - "VRI-com.example/com.example.ui.MainActivity#277" ->
//   "VRI-MainActivity#277"
// - "androidx.compose.ui.platform.ComposeView" -> "ComposeView"
// - SF layers drop the WM wrappers: "ab1254f StatusBar#80" ->
//   "StatusBar#80", "Surface(name=ab1254f StatusBar#80)/@0x31d9daa -
//   animation-leash of insets_animation#271" -> "insets_animation#271"
// Other names ("StatusBar", "DisplayBackGestureHandler 0", "Column") are
// unchanged.
export function displayName(name: string): string {
  let short = name;
  const leash = short.indexOf(LEASH_MARKER);
  if (short.startsWith('Surface(name=') && leash >= 0) {
    short = short.slice(leash + LEASH_MARKER.length);
  }
  const hashed = /^[0-9a-f]{6,8} (.+)$/.exec(short);
  if (hashed !== null) short = hashed[1];
  // Buffer layers prefix the window title ("VRI-", "BBQ-"); kept as is.
  const prefixed = /^([A-Z]{2,}-)(.+)$/.exec(short);
  const prefix = prefixed !== null ? prefixed[1] : '';
  if (prefixed !== null) short = prefixed[2];
  const slash = short.indexOf('/');
  if (slash >= 0 && /^[a-zA-Z]\w*(?:\.\w+)+\//.test(short)) {
    short = short.slice(slash + 1).replace(/^\./, '');
  }
  // SF appends "#<layer id>" to layer names.
  const m = /^(?:[a-z_]\w*\.)+([A-Z][\w$]*(?:#\d+)?)$/.exec(short);
  if (m !== null) short = m[1];
  return prefix + short;
}

export function nodeBounds(n: UiHierarchyNode): Bounds {
  return {
    left: n.boundsLeft,
    top: n.boundsTop,
    right: n.boundsRight,
    bottom: n.boundsBottom,
  };
}

// The properties of a View or Compose node (Window level). States and
// values at their default (not clickable, alpha 1, no elevation, ...) are
// left out.
export function uiNodeRows(n: UiHierarchyNode): PropRow[] {
  const b = new PropRowsBuilder().text('Bounds', formatBounds(nodeBounds(n)), {
    mono: true,
  });
  if (n.isTextRedacted) {
    b.text('Text', 'redacted', {title: 'Password or sensitive input'});
  } else {
    b.copy('Text', n.text);
  }
  b.copy('Content description', n.contentDescription)
    .copy('Test tag', n.testTag)
    .text('Role', n.role)
    .text('State description', n.stateDescription)
    .text('Actions', n.actions)
    .copy('Source', n.sourceLocation)
    .text('Visible', String(n.isVisible));
  if (n.isClickable) b.text('Clickable', 'true');
  if (n.isFocused) b.text('Focused', 'true');
  if (n.alpha !== 1) b.text('Alpha', formatNumber(n.alpha));
  if (n.elevation !== undefined && n.elevation !== 0) {
    b.text('Elevation', `${formatNumber(n.elevation)} dp`);
  }
  if (n.flags !== 0n) {
    b.text('Flags', `0x${n.flags.toString(16)}`, {mono: true});
  }
  const recomposed = n.recompositionCount ?? 0;
  const skipped = n.skipCount ?? 0;
  if (recomposed > 0 || skipped > 0) {
    b.text('Recompositions', `${recomposed} recomposed, ${skipped} skipped`);
  }
  return b.text('Properties', n.properties, {mono: true}).build();
}
