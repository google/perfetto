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
  deserializeStateObject,
  isSerializedBigint,
  serializeStateObject,
} from './upload_utils';

describe('isSerializedBigint', () => {
  it('should return true for a valid serialized bigint', () => {
    const value = {
      __kind: 'bigint',
      value: '1234567890',
    };
    expect(isSerializedBigint(value)).toBeTruthy();
  });

  it('should return false for a null value', () => {
    expect(isSerializedBigint(null)).toBeFalsy();
  });

  it('should return false for a non-object value', () => {
    expect(isSerializedBigint(123)).toBeFalsy();
  });

  it('should return false for a non-serialized bigint value', () => {
    const value = {
      __kind: 'not-bigint',
      value: '1234567890',
    };
    expect(isSerializedBigint(value)).toBeFalsy();
  });
});

describe('serializeStateObject', () => {
  it('should serialize a simple object', () => {
    const object = {
      a: 1,
      b: 2,
      c: 3,
    };
    const expectedJson = `{"a":1,"b":2,"c":3}`;
    expect(serializeStateObject(object)).toEqual(expectedJson);
  });

  it('should serialize a bigint', () => {
    const object = {
      a: 123456789123456789n,
    };
    const expectedJson =
        `{"a":{"__kind":"bigint","value":"123456789123456789"}}`;
    expect(serializeStateObject(object)).toEqual(expectedJson);
  });

  it('should not serialize a non-serializable property', () => {
    const object = {
      a: 1,
      b: 2,
      c: 3,
      nonSerializableState: 4,
    };
    const expectedJson = `{"a":1,"b":2,"c":3}`;
    expect(serializeStateObject(object)).toEqual(expectedJson);
  });
});

describe('deserializeStateObject', () => {
  it('should deserialize a simple object', () => {
    const json = `{"a":1,"b":2,"c":3}`;
    const expectedObject = {
      a: 1,
      b: 2,
      c: 3,
    };
    expect(deserializeStateObject(json)).toEqual(expectedObject);
  });

  it('should deserialize a bigint', () => {
    const json = `{"a":{"__kind":"bigint","value":"123456789123456789"}}`;
    const expectedObject = {
      a: 123456789123456789n,
    };
    expect(deserializeStateObject(json)).toEqual(expectedObject);
  });

  it('should deserialize a null', () => {
    const json = `{"a":null}`;
    const expectedObject = {
      a: null,
    };
    expect(deserializeStateObject(json)).toEqual(expectedObject);
  });
});
