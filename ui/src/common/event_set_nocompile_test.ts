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

import {
  Bool,
  Event,
  EventSet,
  Id,
  KeySet,
  Null,
  Num,
  Str,
  UntypedEventSet,
} from './event_set';

export function eventMustHaveAllKeys(): Event<KeySet> {
  const ks = {
    id: Id,
    foo: Str,
  };

  // @ts-expect-error
  const event: Event<typeof ks> = {
    id: 'myid',
  };

  return event;
}

export function eventMustNotHaveExtraKeys(): Event<KeySet> {
  const ks = {
    id: Id,
    foo: Str,
    bar: Num,
    baz: Bool,
    xyzzy: Null,
  };

  const event: Event<typeof ks> = {
    id: 'myid',
    foo: 'foo',
    bar: 32,
    baz: false,
    xyzzy: null,
    // @ts-expect-error
    plugh: 42,
  };

  return event;
}

export function eventsCanBeWellFormed(): Event<KeySet> {
  const ks = {
    id: Id,
    foo: Str,
    bar: Num,
    baz: Bool,
    xyzzy: Null,
  };

  const event: Event<typeof ks> = {
    id: 'myid',
    foo: 'foo',
    bar: 32,
    baz: false,
    xyzzy: null,
  };

  return event;
}


const lettersKeySet = {
  num: Num,
  char: Str,
};

export async function badMaterialisation(input: EventSet<typeof lettersKeySet>):
    Promise<UntypedEventSet> {
  {
    const a = await input.materialise({
      baz: Num,
    });
    // @ts-expect-error
    a.events;
  }

  {
    // This is fine:
    const a = await input.materialise(lettersKeySet);
    a.events;
  }

  {
    // So is this:
    const a = await input.materialise({
      num: Num,
      char: Str,
    });
    a.events;
  }

  return input;
}
