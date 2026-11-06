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
import {TextInput} from '../../../widgets/text_input';
import {AliasTag} from './alias_tag';

export function sanitizeAlias(value: string): string {
  return value.replace(/[^a-zA-Z0-9_]/g, '_');
}

export interface TextInputWithAliasAttrs {
  readonly value: string;
  readonly onChange: (value: string) => void;
  readonly placeholder?: string;
  readonly alias?: string;
  readonly aliasPlaceholder?: string;
  readonly onAliasChange?: (alias: string) => void;
}

export class TextInputWithAlias implements m.ClassComponent<TextInputWithAliasAttrs> {
  view({attrs}: m.CVnode<TextInputWithAliasAttrs>): m.Children {
    const {
      value,
      onChange,
      placeholder,
      alias,
      aliasPlaceholder,
      onAliasChange,
    } = attrs;

    return [
      m(TextInput, {
        placeholder,
        value,
        onChange,
      }),
      onAliasChange !== undefined &&
        m(AliasTag, {
          alias: alias ?? '',
          placeholder: aliasPlaceholder || sanitizeAlias(value) || 'alias',
          onChange: onAliasChange,
        }),
    ];
  }
}
