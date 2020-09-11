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

import * as protobuf from '../gen/protobufjs-light/protobuf';
import {perfetto} from '../gen/protos';

import {
  DescriptorProto,
  FieldDescriptorProto,
} from './protos';

// Converts FieldDescriptorProto.Type enum to protobufjs friendly strings.
function pbjsFieldType(
    type: FieldDescriptorProto.Type|null|undefined,
    typeName: string|null|undefined): string {
  if (!type) {
    if (!typeName) throw Error('type and type_name cannot both be empty.');
    return typeName;
  }
  switch (type) {
    case FieldDescriptorProto.Type.TYPE_DOUBLE:
      return 'double';
    case FieldDescriptorProto.Type.TYPE_FLOAT:
      return 'float';
    case FieldDescriptorProto.Type.TYPE_INT64:
      return 'int64';
    case FieldDescriptorProto.Type.TYPE_UINT64:
      return 'uint64';
    case FieldDescriptorProto.Type.TYPE_INT32:
      return 'int32';
    case FieldDescriptorProto.Type.TYPE_FIXED64:
      return 'fixed64';
    case FieldDescriptorProto.Type.TYPE_FIXED32:
      return 'fixed32';
    case FieldDescriptorProto.Type.TYPE_BOOL:
      return 'bool';
    case FieldDescriptorProto.Type.TYPE_STRING:
      return 'string';
    case FieldDescriptorProto.Type.TYPE_GROUP:
      throw Error('Field type not supported');
    case FieldDescriptorProto.Type.TYPE_MESSAGE:
      if (!typeName) throw Error('type_name must be set for TYPE_MESSAGE');
      return typeName;
    case FieldDescriptorProto.Type.TYPE_BYTES:
      return 'bytes';
    case FieldDescriptorProto.Type.TYPE_UINT32:
      return 'uint32';
    case FieldDescriptorProto.Type.TYPE_ENUM:
      if (!typeName) throw Error('type_name must be set for TYPE_ENUM');
      return typeName;
    case FieldDescriptorProto.Type.TYPE_SFIXED32:
      return 'sfixed32';
    case FieldDescriptorProto.Type.TYPE_SFIXED64:
      return 'sfixed64';
    case FieldDescriptorProto.Type.TYPE_SINT32:
      return 'sint32';
    case FieldDescriptorProto.Type.TYPE_SINT64:
      return 'sint64';
    default:
      throw Error('Unexpected FieldDescriptorProto.Type');
  }
}

// Converts FieldDescriptorProto.Label enum to protobufjs friendly strings.
function pbjsRuleType(label: FieldDescriptorProto.Label): string {
  switch (label) {
    case FieldDescriptorProto.Label.LABEL_OPTIONAL:
      return 'optional';
    case FieldDescriptorProto.Label.LABEL_REQUIRED:
      return 'required';
    case FieldDescriptorProto.Label.LABEL_REPEATED:
      return 'repeated';
    default:
      throw Error('Unexpected FieldDescriptorProto.Label');
  }
}

// In these "valid" interfaces, the members are no longer optional.
interface ValidDescriptorProto extends DescriptorProto {
  name: string;
}

type IDescriptorProto = perfetto.protos.IDescriptorProto;
type IFieldDescriptorProto = perfetto.protos.IFieldDescriptorProto;

interface ValidFieldDescriptorProto extends FieldDescriptorProto {
  name: string;
  number: number;
  label: perfetto.protos.FieldDescriptorProto.Label;
}

function assertValidDescriptor(descriptor: IDescriptorProto):
    asserts descriptor is ValidDescriptorProto {
  if (!descriptor.name) throw Error('Received descriptor with empty name');
}

function assertValidFieldDescriptor(fieldDesc: IFieldDescriptorProto):
    asserts fieldDesc is ValidFieldDescriptorProto {
  if (!fieldDesc.name) throw Error('Received field with empty name');
  if (!fieldDesc.number) throw Error('Received field with empty number');
  if (!fieldDesc.label) throw Error('Received field with empty label');
}

export function buildMetricProtoNamespace(descriptors: IDescriptorProto[]):
    protobuf.Namespace {
  const root = new protobuf.Root();
  for (const descriptor of descriptors) {
    assertValidDescriptor(descriptor);
    const name = descriptor.name;
    if (name[0] !== '.') throw Error('Descriptor name must start with "."');
    const pieces = name.substring(1).split('.');
    if (pieces.length === 0) throw Error('Invalid descriptor name');

    // If the name is ".perfetto.protos.MyMetric", we first create the
    // namespaces 'perfetto' > 'proto', and then create the type 'MyMetric'
    // in the leaf namespace.
    const namespace = root.define(pieces.slice(0, -1));
    const descType = new protobuf.Type(pieces[pieces.length - 1]);
    namespace.add(descType);

    for (const field of descriptor.field) {
      assertValidFieldDescriptor(field);
      const fieldType = pbjsFieldType(field.type, field.typeName);
      const rule = pbjsRuleType(field.label);
      descType.add(
          new protobuf.Field(field.name, field.number, fieldType, rule));
    }
  }
  return root;
}
