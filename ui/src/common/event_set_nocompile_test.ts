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
  BoolType,
  Event,
  IdType,
  KeySet,
  NullType,
  NumType,
  StrType,
} from './event_set';

export function keySetMustHaveId(): KeySet {
  // @ts-expect-error
  const ks: KeySet = {};
  return ks;
}

export function keySetMustHaveCorrectIdType(): KeySet {
  const ks: KeySet = {
    // @ts-expect-error
    id: StrType,
  };
  return ks;
}

export function eventMustHaveAllKeys(): Event<KeySet> {
  const ks = {
    id: IdType,
    foo: StrType,
  };

  // @ts-expect-error
  const event: Event<typeof ks> = {
    id: 'myid',
  };

  return event;
}

export function eventMayHaveNonKeyTypeValues(): Event<KeySet> {
  const ks = {
    id: IdType,
    foo: StrType,
    bar: NumType,
    baz: BoolType,
    xyzzy: NullType,
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
