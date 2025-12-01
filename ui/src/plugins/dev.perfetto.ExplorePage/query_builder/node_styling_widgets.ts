// Copyright (C) 2025 The Android Open Source Project
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
import {classNames} from '../../../base/classnames';
import {TextInput, TextInputAttrs} from '../../../widgets/text_input';

// Helper functions and components for consistent node details styling
//
// Usage Guide:
// - NodeDetailsContent(): Wrapper for all node details content - use as the outermost container
// - NodeTitle(): Display the node's title in bold with larger font
// - NodeDetailsMessage(): Show informational messages (e.g., "No columns added")
// - NodeDetailsText(): Display secondary text with lighter color
// - ColumnName(): Display column names with monospace font and background
// - NodeDetailsSpacer(): Add consistent vertical spacing between sections
// - NarrowTextInput: Component for compact number inputs (e.g., limit, offset values)
//
// Example:
//   nodeDetails(): NodeDetailsAttrs {
//     return {
//       content: NodeDetailsContent([
//         NodeTitle('My Node'),
//         ColumnName('column_name'),
//         NodeDetailsMessage('No filters applied'),
//       ]),
//     };
//   }

// Wrapper for node details content - provides consistent layout and spacing
export function NodeDetailsContent(children: m.Children): m.Child {
  return m('.pf-exp-node-details-content', children);
}

// Title for nodes - bold, larger font
export function NodeTitle(title: string): m.Child {
  return m('.pf-exp-node-details-title', title);
}

// Message for informational text (e.g., "No columns added")
export function NodeDetailsMessage(message: string): m.Child {
  return m('.pf-exp-node-details-message', message);
}

// Text with secondary/lighter color
export function NodeDetailsText(children: m.Children): m.Child {
  return m('.pf-exp-node-details-text', children);
}

// Spacer for consistent vertical spacing
export function NodeDetailsSpacer(): m.Child {
  return m('.pf-exp-node-details-spacer');
}

// Column name styling - monospace font with subtle background
export function ColumnName(name: string): m.Child {
  return m('code.pf-exp-column-name', name);
}

// Narrow text input for compact number inputs - wraps TextInput from src/widgets
export interface NarrowTextInputAttrs extends TextInputAttrs {
  className?: string;
}

export class NarrowTextInput implements m.ClassComponent<NarrowTextInputAttrs> {
  view({attrs}: m.CVnode<NarrowTextInputAttrs>) {
    const {className, ...textInputAttrs} = attrs;
    return m(TextInput, {
      ...textInputAttrs,
      className: classNames('pf-exp-node-details-input-narrow', className),
    });
  }
}
