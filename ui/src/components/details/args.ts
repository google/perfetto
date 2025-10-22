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
import {ArgNode, convertArgsToTree, Key} from './slice_args_parser';
import {Anchor} from '../../widgets/anchor';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {TreeNode} from '../../widgets/tree';
import {Arg} from '../sql_utils/args';
import {Trace} from '../../public/trace';

// Renders slice arguments (key/value pairs) as a subtree.
export function renderArguments(
  trace: Trace,
  args: ReadonlyArray<Arg>,
  extraMenuItems?: (arg: Arg) => m.Children,
): m.Children {
  if (args.length > 0) {
    const tree = convertArgsToTree(args);
    return renderArgTreeNodes(trace, tree, extraMenuItems);
  } else {
    return undefined;
  }
}

export function hasArgs(args?: Arg[]): args is Arg[] {
  return exists(args) && args.length > 0;
}

function renderArgTreeNodes(
  trace: Trace,
  args: ArgNode<Arg>[],
  extraMenuItems?: (arg: Arg) => m.Children,
): m.Children {
  return args.map((arg) => {
    const {key, value, children} = arg;
    if (children && children.length === 1) {
      // If we only have one child, collapse into self and combine keys
      const child = children[0];
      const compositeArg = {
        ...child,
        key: stringifyKey(key, child.key),
      };
      return renderArgTreeNodes(trace, [compositeArg], extraMenuItems);
    } else {
      return m(
        TreeNode,
        {
          left: renderArgKey(stringifyKey(key), value, extraMenuItems),
          right: exists(value) && renderArgValue(value),
          summary: children && renderSummary(children),
        },
        children && renderArgTreeNodes(trace, children, extraMenuItems),
      );
    }
  });
}

function renderArgKey(
  key: string,
  value: Arg | undefined,
  extraMenuItems?: (arg: Arg) => m.Children,
): m.Children {
  if (value === undefined) {
    return key;
  } else {
    const {key: fullKey} = value;
    return m(
      PopupMenu,
      {trigger: m(Anchor, {icon: Icons.ContextMenu}, key)},
      m(MenuItem, {
        label: 'Copy full key',
        icon: 'content_copy',
        onclick: () => navigator.clipboard.writeText(fullKey),
      }),
      extraMenuItems?.(value),
    );
  }
}

function renderArgValue({displayValue}: Arg): m.Children {
  if (isWebLink(displayValue)) {
    return renderWebLink(displayValue);
  } else {
    return `${displayValue}`;
  }
}

function renderSummary(children: ArgNode<Arg>[]): m.Children {
  const summary = children
    .slice(0, 2)
    .map(({key}) => key)
    .join(', ');
  const remaining = children.length - 2;
  if (remaining > 0) {
    return `{${summary}, ... (${remaining} more items)}`;
  } else {
    return `{${summary}}`;
  }
}

function stringifyKey(...key: Key[]): string {
  return key
    .map((element, index) => {
      if (typeof element === 'number') {
        return `[${element}]`;
      } else {
        return (index === 0 ? '' : '.') + element;
      }
    })
    .join('');
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
