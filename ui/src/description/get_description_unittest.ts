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

import {getDescription, handleArgs} from './get_description';
import {Description, DescriptionState} from './description_state';
import {enableMapSet} from 'immer';
import {asArgId} from '../components/sql_utils/core_types';
import {Arg} from '../components/sql_utils/args';

function strToReg(str: string): RegExp | string {
  try {
    return eval(str);
  } catch (error) {
    return str;
  }
}

function addDescription(desc: Description[]) {
  DescriptionState.edit((draft) => {
    desc.forEach((desc) => {
      let name: string | RegExp = desc.name;
      if (name.startsWith('/')) {
        name = strToReg(name);
      }
      if (name instanceof RegExp) {
        draft.descReg.set(name, desc.description);
      } else {
        draft.descStr.set(name, desc.description);
      }
    });
  });
}

describe('getDescription', () => {
  beforeAll(() => {
    enableMapSet();
    const desc: Description[] = [
      {
        name: '/^test/',
        description: '/^test/',
      },
      {
        name: '/hello/',
        description: '/hello/',
      },
      {
        name: '/world$/',
        description: '/world$/',
      },
      {
        name: 'foo',
        description: 'foo',
      },
      {
        name: 'bar',
        description: 'bar',
      },
      {
        name: 'baz',
        description: 'baz',
      },
      {
        name: 'test',
        description: 'test',
      },
    ];
    addDescription(desc);
  });

  test('should return "" when name is undefined', () => {
    const result = getDescription(undefined);
    expect(result).toBe('');
  });

  test('should return "" when name is an empty string', () => {
    const result = getDescription('');
    expect(result).toBe('');
  });

  test('should return the matched regex pattern when name matches a regex', () => {
    const result = getDescription('test123'); // Matches /^test/
    expect(result).toBe('/^test/');
  });

  test('should return the matched string when name is in the string array', () => {
    const result = getDescription('foo'); // Matches 'foo' in stringArray
    expect(result).toBe('foo');
  });

  test('should return "" when name matches neither regex nor string array', () => {
    const result = getDescription('nonexistent');
    expect(result).toBe('');
  });

  test('should give precedence to string match over regex match', () => {
    const result = getDescription('test');
    expect(result).toBe('test');
  });
});

describe('handle description args', () => {
  const description =
    'This is a handleArgs test description @args{arg1}, @args{arga2}';
  test('handle args null', () => {
    const result = handleArgs(description);
    expect(result).toEqual(description);
  });

  test('handle proto trace args', () => {
    const args: Arg[] = [
      {
        id: asArgId(0),
        flatKey: 'debug.arg1',
        key: 'debug.arg1',
        value: 'value1',
        displayValue: 'arg1',
      },
      {
        id: asArgId(1),
        flatKey: 'debug.arg2',
        key: 'debug.arg2',
        value: 'value2',
        displayValue: 'arg1',
      },
    ];
    const result = handleArgs(description, args);
    expect(result).toEqual(
      description
        .replace('@args{arg1}', 'value1')
        .replace('@args{arg2}', 'value2'),
    );
  });

  test('handle json trace args', () => {
    const args: Arg[] = [
      {
        id: asArgId(0),
        flatKey: 'args.arg1',
        key: 'args.arg1',
        value: 'value1',
        displayValue: 'arg1',
      },
      {
        id: asArgId(1),
        flatKey: 'args.arg2',
        key: 'args.arg2',
        value: 'value2',
        displayValue: 'arg1',
      },
    ];
    const result = handleArgs(description, args);
    expect(result).toEqual(
      description
        .replace('@args{arg1}', 'value1')
        .replace('@args{arg2}', 'value2'),
    );
  });
});

describe('handleArgs for json trace', () => {
  test('should return orginal desc when args is undefined', () => {
    const result = handleArgs('Hello @args{name}!', undefined);
    expect(result).toBe('Hello @args{name}!');
  });

  test('should handle empty args array correctly', () => {
    const text = 'Hello @args{name}!';
    const args: Arg[] = [];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello @args{name}!');
  });

  test('should replace @args{name} with the corresponding value in args', () => {
    const text = 'Hello @args{name}!';
    const args: Arg[] = [
      {
        id: asArgId(0),
        flatKey: 'args.name',
        key: 'args.name',
        value: 'World',
        displayValue: 'name',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello World!');
  });

  test('should replace multiple @args{name} with corresponding values', () => {
    const text = 'Hello @args{name}, welcome to @args{place}!';
    const args: Arg[] = [
      {
        id: asArgId(0),
        flatKey: 'args.name',
        key: 'args.name',
        value: 'Alice',
        displayValue: 'name',
      },
      {
        id: asArgId(1),
        flatKey: 'args.place',
        key: 'args.place',
        value: 'Wonderland',
        displayValue: 'place',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello Alice, welcome to Wonderland!');
  });

  test('should leave @args{name} intact if no matching value is found', () => {
    const text = 'Hello @args{name}!';
    const args: Arg[] = [
      {
        id: asArgId(1),
        flatKey: 'args.place',
        key: 'args.place',
        value: 'Wonderland',
        displayValue: 'place',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello @args{name}!');
  });

  test('should handle text with no @args{name} placeholders', () => {
    const text = 'Hello World!';
    const args: Arg[] = [
      {
        id: asArgId(1),
        flatKey: 'args.place',
        key: 'args.place',
        value: 'Wonderland',
        displayValue: 'place',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello World!');
  });

  test('should handle empty text correctly', () => {
    const text = '';
    const args: Arg[] = [
      {
        id: asArgId(1),
        flatKey: 'args.place',
        key: 'args.place',
        value: 'Wonderland',
        displayValue: 'place',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('');
  });

  test('should handle multiple occurrences of the same placeholder', () => {
    const text = '@args{name} and @args{name} are friends.';
    const args: Arg[] = [
      {
        id: asArgId(0),
        flatKey: 'args.name',
        key: 'args.name',
        value: 'Alice',
        displayValue: 'name',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Alice and Alice are friends.');
  });
});

describe('handleArgs for proto trace', () => {
  test('should return orginal desc when args is undefined', () => {
    const result = handleArgs('Hello @args{name}!', undefined);
    expect(result).toBe('Hello @args{name}!');
  });

  test('should handle empty args array correctly', () => {
    const text = 'Hello @args{name}!';
    const args: Arg[] = [];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello @args{name}!');
  });

  test('should replace @args{name} with the corresponding value in args', () => {
    const text = 'Hello @args{name}!';
    const args: Arg[] = [
      {
        id: asArgId(0),
        flatKey: 'debug.name',
        key: 'debug.name',
        value: 'World',
        displayValue: 'name',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello World!');
  });

  test('should replace multiple @args{name} with corresponding values', () => {
    const text = 'Hello @args{name}, welcome to @args{place}!';
    const args: Arg[] = [
      {
        id: asArgId(0),
        flatKey: 'debug.name',
        key: 'debug.name',
        value: 'Alice',
        displayValue: 'name',
      },
      {
        id: asArgId(1),
        flatKey: 'debug.place',
        key: 'debug.place',
        value: 'Wonderland',
        displayValue: 'place',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello Alice, welcome to Wonderland!');
  });

  test('should leave @args{name} intact if no matching value is found', () => {
    const text = 'Hello @args{name}!';
    const args: Arg[] = [
      {
        id: asArgId(1),
        flatKey: 'debug.place',
        key: 'debug.place',
        value: 'Wonderland',
        displayValue: 'place',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello @args{name}!');
  });

  test('should handle text with no @args{name} placeholders', () => {
    const text = 'Hello World!';
    const args: Arg[] = [
      {
        id: asArgId(1),
        flatKey: 'debug.place',
        key: 'debug.place',
        value: 'Wonderland',
        displayValue: 'place',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Hello World!');
  });

  test('should handle empty text correctly', () => {
    const text = '';
    const args: Arg[] = [
      {
        id: asArgId(1),
        flatKey: 'debug.place',
        key: 'debug.place',
        value: 'Wonderland',
        displayValue: 'place',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('');
  });

  test('should handle multiple occurrences of the same placeholder', () => {
    const text = '@args{name} and @args{name} are friends.';
    const args: Arg[] = [
      {
        id: asArgId(0),
        flatKey: 'debug.name',
        key: 'debug.name',
        value: 'Alice',
        displayValue: 'name',
      },
    ];
    const result = handleArgs(text, args);
    expect(result).toBe('Alice and Alice are friends.');
  });
});
