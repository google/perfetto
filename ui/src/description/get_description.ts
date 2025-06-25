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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Arg, ArgValue} from '../components/sql_utils/args';
import {DescriptionState} from './description_state';

export function handleArgs(description: string, args?: Arg[]): string {
  let result = description;
  if (!args || args.length === 0) {
    return result;
  }

  const regex = /@args\{([^\}]*)\}/g;

  const matches: string[] = [];
  let match: RegExpExecArray | null;

  while ((match = regex.exec(description)) !== null) {
    matches.push(match[1]);
  }

  if (matches.length === 0) {
    return result;
  }

  const argMapping: Map<string, ArgValue> = new Map();
  args.forEach((arg) => {
    let name = '';
    const debugPrefix = 'debug.';
    const argsPrefix = 'args.';
    if (arg.key.startsWith(debugPrefix)) {
      name = arg.key.substring(debugPrefix.length);
    } else if (arg.key.startsWith(argsPrefix)) {
      name = arg.key.substring(argsPrefix.length);
    }

    argMapping.set(name, arg.value);
  });

  matches.forEach((match) => {
    const argValue = argMapping.get(match);
    if (argValue !== undefined && argValue !== null) {
      result = result.replace(`@args{${match}}`, argValue.toString());
    }
  });

  return result;
}

export function getDescription(name?: string, args?: Arg[]): string {
  if (!name) {
    return '';
  }

  let description = DescriptionState.state.descStr.get(name);

  if (!description) {
    DescriptionState.state.descReg.forEach((desc, reg) => {
      if (reg.test(name)) {
        description = desc;
      }
    });
  }

  if (description) {
    return handleArgs(description ?? '', args);
  }
  return '';
}
