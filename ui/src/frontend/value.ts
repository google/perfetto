// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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
import {Tree, TreeNode} from '../widgets/tree';
import {PopupMenuButton, PopupMenuItem} from './popup_menu';

// This file implements a component for rendering JSON-like values (with
// customisation options like context menu and action buttons).
//
// It defines the common Value, StringValue, DictValue, ArrayValue types,
// to be used as an interchangeable format between different components
// and `renderValue` function to convert DictValue into vdom nodes.

// Leaf (non-dict and non-array) value which can be displayed to the user
// together with the rendering customisation parameters.
type StringValue = {
  kind: 'STRING';
  value: string;
} & StringValueParams;

// Helper function to create a StringValue from string together with optional
// parameters.
export function value(value: string, params?: StringValueParams): StringValue {
  return {
    kind: 'STRING',
    value,
    ...params,
  };
}

// Helper function to convert a potentially undefined value to StringValue or
// null.
export function maybeValue(
  v?: string,
  params?: StringValueParams,
): StringValue | null {
  if (!v) {
    return null;
  }
  return value(v, params);
}

// A basic type for the JSON-like value, comprising a primitive type (string)
// and composite types (arrays and dicts).
export type Value = StringValue | Array | Dict;

// Dictionary type.
export type Dict = {
  kind: 'DICT';
  items: {[name: string]: Value};
} & ValueParams;

// Helper function to simplify creation of a dictionary.
// This function accepts and filters out nulls as values in the passed
// dictionary (useful for simplifying the code to render optional values).
export function dict(
  items: {[name: string]: Value | null},
  params?: ValueParams,
): Dict {
  const result: {[name: string]: Value} = {};
  for (const [name, value] of Object.entries(items)) {
    if (value !== null) {
      result[name] = value;
    }
  }
  return {
    kind: 'DICT',
    items: result,
    ...params,
  };
}

// Array type.
export type Array = {
  kind: 'ARRAY';
  items: Value[];
} & ValueParams;

// Helper function to simplify creation of an array.
// This function accepts and filters out nulls in the passed array (useful for
// simplifying the code to render optional values).
export function array(items: (Value | null)[], params?: ValueParams): Array {
  return {
    kind: 'ARRAY',
    items: items.filter((item: Value | null) => item !== null) as Value[],
    ...params,
  };
}

// Parameters for displaying a button next to a value to perform
// the context-dependent action (i.e. go to the corresponding slice).
type ButtonParams = {
  action: () => void;
  hoverText?: string;
  icon?: string;
};

// Customisation parameters which apply to any Value (e.g. context menu).
interface ValueParams {
  contextMenu?: PopupMenuItem[];
}

// Customisation parameters which apply for a primitive value (e.g. showing
// button next to a string, or making it clickable, or adding onhover effect).
interface StringValueParams extends ValueParams {
  leftButton?: ButtonParams;
  rightButton?: ButtonParams;
}

export function isArray(value: Value): value is Array {
  return value.kind === 'ARRAY';
}

export function isDict(value: Value): value is Dict {
  return value.kind === 'DICT';
}

export function isStringValue(value: Value): value is StringValue {
  return !isArray(value) && !isDict(value);
}

// Recursively render the given value and its children, returning a list of
// vnodes corresponding to the nodes of the table.
function renderValue(name: string, value: Value): m.Children {
  const left = [
    name,
    value.contextMenu
      ? m(PopupMenuButton, {
          icon: 'arrow_drop_down',
          items: value.contextMenu,
        })
      : null,
  ];
  if (isArray(value)) {
    const nodes = value.items.map((value: Value, index: number) => {
      return renderValue(`[${index}]`, value);
    });
    return m(TreeNode, {left, right: `array[${nodes.length}]`}, nodes);
  } else if (isDict(value)) {
    const nodes: m.Children[] = [];
    for (const key of Object.keys(value.items)) {
      nodes.push(renderValue(key, value.items[key]));
    }
    return m(TreeNode, {left, right: `dict`}, nodes);
  } else {
    const renderButton = (button?: ButtonParams) => {
      if (!button) {
        return null;
      }
      return m(
        'i.material-icons.grey',
        {
          onclick: button.action,
          title: button.hoverText,
        },
        button.icon ?? 'call_made',
      );
    };
    if (value.kind === 'STRING') {
      const right = [
        renderButton(value.leftButton),
        m('span', value.value),
        renderButton(value.rightButton),
      ];
      return m(TreeNode, {left, right});
    } else {
      return null;
    }
  }
}

// Render a given dictionary to a tree.
export function renderDict(dict: Dict): m.Child {
  const rows: m.Children[] = [];
  for (const key of Object.keys(dict.items)) {
    rows.push(renderValue(key, dict.items[key]));
  }
  return m(Tree, rows);
}
