// Copyright (C) 2023 The Android Open Source Project
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
import {isString} from '../../base/object_utils';
import {Icons} from '../../base/semantic_icons';
import {exists} from '../../base/utils';
import {Anchor} from '../../widgets/anchor';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {TreeNode} from '../../widgets/tree';
import type {Args, ArgsDict, ArgValue} from '../sql_utils/args';
import type {Trace} from '../../public/trace';
import {STR_NULL} from '../../trace_processor/query_result';

// An arg that trace_processor tagged as a reference to a row in another table:
// a upid from an (is_pid) annotation, or a utid from (is_tid). The `__ref`
// names the referenced table and `id` is the row id.
interface ArgRef {
  readonly __ref: string;
  readonly id: bigint;
}

function isArgRef(value: unknown): value is ArgRef {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    typeof (value as {__ref?: unknown}).__ref === 'string' &&
    'id' in value
  );
}

// The `<table, id column>` for a reference table, or undefined if we don't know
// how to resolve a name for it.
function refNameSource(
  refTable: string,
): {table: string; idCol: string} | undefined {
  if (refTable === 'process') return {table: 'process', idCol: 'upid'};
  if (refTable === 'thread') return {table: 'thread', idCol: 'utid'};
  return undefined;
}

// The key of the companion name row derived from a reference's key:
// caller_upid -> caller_process_name, render_utid -> render_thread_name. The
// suffix is guaranteed by trace_processor; returns undefined (skip the name
// row) if the table is unknown or the key does not end in the expected suffix.
function refNameKey(refTable: string, key: string): string | undefined {
  if (refTable === 'process' && key.endsWith('upid')) {
    return key.slice(0, -'upid'.length) + 'process_name';
  }
  if (refTable === 'thread' && key.endsWith('utid')) {
    return key.slice(0, -'utid'.length) + 'thread_name';
  }
  return undefined;
}

interface ArgRefNameAttrs {
  readonly trace: Trace;
  readonly ref: ArgRef;
}

// Renders the process/thread name (without the pid/tid suffix) for a reference
// arg. The name is resolved live from the process/thread table, so it never
// goes stale; falls back to the raw id if the entity has no name.
class ArgRefName implements m.ClassComponent<ArgRefNameAttrs> {
  private name?: string;
  private done = false;

  oninit({attrs}: m.Vnode<ArgRefNameAttrs>) {
    const src = refNameSource(attrs.ref.__ref);
    if (src === undefined) {
      this.done = true;
      return;
    }
    attrs.trace.engine
      .query(
        `select name from ${src.table} where ${src.idCol} = ${attrs.ref.id} limit 1`,
      )
      .then((res) => {
        const it = res.iter({name: STR_NULL});
        if (it.valid() && it.name !== null) this.name = it.name;
      })
      .catch(() => {})
      .finally(() => {
        this.done = true;
        m.redraw();
      });
  }

  view({attrs}: m.Vnode<ArgRefNameAttrs>): m.Children {
    if (this.name !== undefined) return this.name;
    return this.done ? `${attrs.ref.id}` : '';
  }
}

// Renders slice arguments (key/value pairs) as a subtree.
export function renderArguments(
  trace: Trace,
  args: ArgsDict,
  extraMenuItems?: (key: string, arg: ArgValue) => m.Children,
): m.Children {
  if (hasArgs(args)) {
    return Object.entries(args).map(([key, value]) =>
      renderArgsTree(trace, key, key, value, extraMenuItems),
    );
  }
  return undefined;
}

export function hasArgs(args?: ArgsDict): args is ArgsDict {
  return exists(args) && Object.keys(args).length > 0;
}

function renderArgsTree(
  trace: Trace,
  key: string,
  fullKey: string,
  args: Args,
  extraMenuItems?: (path: string, arg: ArgValue) => m.Children,
): m.Children {
  if (args instanceof Array) {
    return m(
      TreeNode,
      {
        left: key,
        summary: renderArraySummary(args),
      },
      args.map((value, index) =>
        renderArgsTree(
          trace,
          `[${index}]`,
          `${fullKey}[${index}]`,
          value,
          extraMenuItems,
        ),
      ),
    );
  }
  if (isArgRef(args)) {
    // Leave the upid/utid arg as-is (the raw id), and, when the key carries the
    // expected suffix, add a companion row with the entity's name.
    const nodes: m.Children[] = [
      m(TreeNode, {
        left: renderArgKey(key, fullKey, args.id, extraMenuItems),
        right: `${args.id}`,
      }),
    ];
    const nameKey = refNameKey(args.__ref, key);
    if (nameKey !== undefined) {
      const nameFullKey =
        fullKey.slice(0, fullKey.length - key.length) + nameKey;
      nodes.push(
        m(TreeNode, {
          left: renderArgKey(nameKey, nameFullKey, null, extraMenuItems),
          right: m(ArgRefName, {trace, ref: args}),
        }),
      );
    }
    return nodes;
  }
  if (args !== null && typeof args === 'object') {
    if (Object.keys(args).length === 1) {
      const [[childName, value]] = Object.entries(args);
      return renderArgsTree(
        trace,
        `${key}.${childName}`,
        `${fullKey}.${childName}`,
        value,
        extraMenuItems,
      );
    }
    return m(
      TreeNode,
      {
        left: key,
        summary: renderDictSummary(args),
      },
      Object.entries(args).map(([childName, child]) =>
        renderArgsTree(
          trace,
          childName,
          `${fullKey}.${childName}`,
          child,
          extraMenuItems,
        ),
      ),
    );
  }
  return m(TreeNode, {
    left: renderArgKey(key, fullKey, args, extraMenuItems),
    right: renderArgValue(args),
  });
}

function renderArgKey(
  key: string,
  fullKey: string,
  value: ArgValue,
  extraMenuItems?: (path: string, arg: ArgValue) => m.Children,
): m.Children {
  if (value === undefined) {
    return key;
  } else {
    return m(
      PopupMenu,
      {trigger: m(Anchor, {icon: Icons.ContextMenu}, key)},
      m(MenuItem, {
        label: 'Copy full key',
        icon: 'content_copy',
        onclick: () => navigator.clipboard.writeText(fullKey),
      }),
      extraMenuItems?.(fullKey, value),
    );
  }
}

function renderArgValue(value: ArgValue): m.Children {
  if (isWebLink(value)) {
    return renderWebLink(value);
  } else {
    return `${value}`;
  }
}

function renderArraySummary(children: Args[]): m.Children {
  return `[ ... (${children.length} items) ]`;
}

function renderDictSummary(children: ArgsDict): m.Children {
  const summary = Object.keys(children).slice(0, 2).join(', ');
  const remaining = Object.keys(children).length - 2;
  if (remaining > 0) {
    return `{${summary}, ... (${remaining} more items)}`;
  } else {
    return `{${summary}}`;
  }
}

function isWebLink(value: unknown): value is string {
  return (
    isString(value) &&
    (value.startsWith('http://') || value.startsWith('https://'))
  );
}

function renderWebLink(url: string): m.Children {
  return m(Anchor, {href: url, target: '_blank', icon: 'open_in_new'}, url);
}
