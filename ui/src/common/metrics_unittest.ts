// Copyright (C) 2020 The Android Open Source Project
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

import {perfetto} from '../gen/protos';

import {buildMetricProtoNamespace} from './metrics';
import {FieldDescriptorProto} from './protos';

// Some shorthands and a convenience function for more readable test proto
// message definitions. The field "optional string foo = 1" now becomes
// "fld(optional, tString, 'foo', 1)".
const optional = FieldDescriptorProto.Label.LABEL_OPTIONAL;
const repeated = FieldDescriptorProto.Label.LABEL_REPEATED;
const tString = FieldDescriptorProto.Type.TYPE_STRING;
const tDouble = FieldDescriptorProto.Type.TYPE_DOUBLE;
const tInt64 = FieldDescriptorProto.Type.TYPE_INT64;

function fld(
    label: FieldDescriptorProto.Label,
    fieldType: FieldDescriptorProto.Type|string,
    name: string,
    fieldNumber: number): perfetto.protos.IFieldDescriptorProto {
  const type = (typeof fieldType === 'string') ?
      FieldDescriptorProto.Type.TYPE_MESSAGE :
      fieldType;
  const typeName = (typeof fieldType === 'string') ? fieldType : undefined;
  return {label, type, name, number: fieldNumber, typeName};
}

test('handles simple proto message', () => {
  const descriptor = {
    name: '.MyProto',
    field: [
      fld(optional, tString, 'string_field', 1),
      fld(optional, tDouble, 'double_field', 2),
    ]
  };

  const namespace = buildMetricProtoNamespace([descriptor]);
  const myProto = namespace.lookupType('.MyProto');
  const jsonMsg = {string_field: 'hello', double_field: 42.324};
  const encoded = myProto.encode(jsonMsg).finish();
  const decoded = myProto.decode(encoded);
  expect(decoded).toEqual(jsonMsg);
});

test('handles repeated field', () => {
  const descriptor = {
    name: '.MyProto',
    field: [fld(repeated, tString, 'string_field', 1)]
  };

  const namespace = buildMetricProtoNamespace([descriptor]);
  const myProto = namespace.lookupType('.MyProto');
  const jsonMsg = {string_field: ['aaa', 'bbb']};
  const encoded = myProto.encode(jsonMsg).finish();
  const decoded = myProto.decode(encoded);
  expect(decoded).toEqual(jsonMsg);
});

test('handles proto message with package', () => {
  const descriptor = {
    name: '.MyPackage.MyProto',
    field: [fld(optional, tString, 'string_field', 1)]
  };

  const namespace = buildMetricProtoNamespace([descriptor]);
  expect(namespace.lookup('MyPackage')).not.toBeNull();
  const myProto = namespace.lookupType('.MyPackage.MyProto');
  const jsonMsg = {string_field: 'hello'};
  const encoded = myProto.encode(jsonMsg).finish();
  const decoded = myProto.decode(encoded);
  expect(decoded).toEqual(jsonMsg);
});

test('cross links message definitions', () => {
  const fooDesc = {
    name: '.FooPackage.NestedProto',
    field: [fld(optional, tString, 'foo', 1)]
  };

  const barDesc = {
    name: '.BarPackage.NestedProto',
    field: [fld(optional, tInt64, 'bar', 1)]
  };

  const myProtoDesc = {
    name: '.MyProto',
    field: [
      fld(optional, '.FooPackage.NestedProto', 'nested_foo', 1),
      fld(repeated, '.BarPackage.NestedProto', 'nested_bar', 2),
    ]
  };

  const namespace = buildMetricProtoNamespace([myProtoDesc, fooDesc, barDesc]);
  const myProto = namespace.lookupType('.MyProto');
  const jsonMsg = {
    nested_foo: {foo: 'hello'},
    nested_bar: [{bar: 42}, {bar: 43}],
  };
  const encoded = myProto.encode(jsonMsg).finish();
  const decoded = myProto.decode(encoded);
  expect(decoded).toEqual(jsonMsg);
});
