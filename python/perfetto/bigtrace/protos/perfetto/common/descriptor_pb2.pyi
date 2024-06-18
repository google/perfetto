from google.protobuf.internal import containers as _containers
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf.internal import python_message as _python_message
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Iterable as _Iterable, Mapping as _Mapping, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class FileDescriptorSet(_message.Message):
    __slots__ = ("file",)
    FILE_FIELD_NUMBER: _ClassVar[int]
    file: _containers.RepeatedCompositeFieldContainer[FileDescriptorProto]
    def __init__(self, file: _Optional[_Iterable[_Union[FileDescriptorProto, _Mapping]]] = ...) -> None: ...

class FileDescriptorProto(_message.Message):
    __slots__ = ("name", "package", "dependency", "public_dependency", "weak_dependency", "message_type", "enum_type", "extension")
    NAME_FIELD_NUMBER: _ClassVar[int]
    PACKAGE_FIELD_NUMBER: _ClassVar[int]
    DEPENDENCY_FIELD_NUMBER: _ClassVar[int]
    PUBLIC_DEPENDENCY_FIELD_NUMBER: _ClassVar[int]
    WEAK_DEPENDENCY_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_TYPE_FIELD_NUMBER: _ClassVar[int]
    ENUM_TYPE_FIELD_NUMBER: _ClassVar[int]
    EXTENSION_FIELD_NUMBER: _ClassVar[int]
    name: str
    package: str
    dependency: _containers.RepeatedScalarFieldContainer[str]
    public_dependency: _containers.RepeatedScalarFieldContainer[int]
    weak_dependency: _containers.RepeatedScalarFieldContainer[int]
    message_type: _containers.RepeatedCompositeFieldContainer[DescriptorProto]
    enum_type: _containers.RepeatedCompositeFieldContainer[EnumDescriptorProto]
    extension: _containers.RepeatedCompositeFieldContainer[FieldDescriptorProto]
    def __init__(self, name: _Optional[str] = ..., package: _Optional[str] = ..., dependency: _Optional[_Iterable[str]] = ..., public_dependency: _Optional[_Iterable[int]] = ..., weak_dependency: _Optional[_Iterable[int]] = ..., message_type: _Optional[_Iterable[_Union[DescriptorProto, _Mapping]]] = ..., enum_type: _Optional[_Iterable[_Union[EnumDescriptorProto, _Mapping]]] = ..., extension: _Optional[_Iterable[_Union[FieldDescriptorProto, _Mapping]]] = ...) -> None: ...

class DescriptorProto(_message.Message):
    __slots__ = ("name", "field", "extension", "nested_type", "enum_type", "oneof_decl", "reserved_range", "reserved_name")
    class ReservedRange(_message.Message):
        __slots__ = ("start", "end")
        START_FIELD_NUMBER: _ClassVar[int]
        END_FIELD_NUMBER: _ClassVar[int]
        start: int
        end: int
        def __init__(self, start: _Optional[int] = ..., end: _Optional[int] = ...) -> None: ...
    NAME_FIELD_NUMBER: _ClassVar[int]
    FIELD_FIELD_NUMBER: _ClassVar[int]
    EXTENSION_FIELD_NUMBER: _ClassVar[int]
    NESTED_TYPE_FIELD_NUMBER: _ClassVar[int]
    ENUM_TYPE_FIELD_NUMBER: _ClassVar[int]
    ONEOF_DECL_FIELD_NUMBER: _ClassVar[int]
    RESERVED_RANGE_FIELD_NUMBER: _ClassVar[int]
    RESERVED_NAME_FIELD_NUMBER: _ClassVar[int]
    name: str
    field: _containers.RepeatedCompositeFieldContainer[FieldDescriptorProto]
    extension: _containers.RepeatedCompositeFieldContainer[FieldDescriptorProto]
    nested_type: _containers.RepeatedCompositeFieldContainer[DescriptorProto]
    enum_type: _containers.RepeatedCompositeFieldContainer[EnumDescriptorProto]
    oneof_decl: _containers.RepeatedCompositeFieldContainer[OneofDescriptorProto]
    reserved_range: _containers.RepeatedCompositeFieldContainer[DescriptorProto.ReservedRange]
    reserved_name: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, name: _Optional[str] = ..., field: _Optional[_Iterable[_Union[FieldDescriptorProto, _Mapping]]] = ..., extension: _Optional[_Iterable[_Union[FieldDescriptorProto, _Mapping]]] = ..., nested_type: _Optional[_Iterable[_Union[DescriptorProto, _Mapping]]] = ..., enum_type: _Optional[_Iterable[_Union[EnumDescriptorProto, _Mapping]]] = ..., oneof_decl: _Optional[_Iterable[_Union[OneofDescriptorProto, _Mapping]]] = ..., reserved_range: _Optional[_Iterable[_Union[DescriptorProto.ReservedRange, _Mapping]]] = ..., reserved_name: _Optional[_Iterable[str]] = ...) -> None: ...

class UninterpretedOption(_message.Message):
    __slots__ = ("name", "identifier_value", "positive_int_value", "negative_int_value", "double_value", "string_value", "aggregate_value")
    class NamePart(_message.Message):
        __slots__ = ("name_part", "is_extension")
        NAME_PART_FIELD_NUMBER: _ClassVar[int]
        IS_EXTENSION_FIELD_NUMBER: _ClassVar[int]
        name_part: str
        is_extension: bool
        def __init__(self, name_part: _Optional[str] = ..., is_extension: bool = ...) -> None: ...
    NAME_FIELD_NUMBER: _ClassVar[int]
    IDENTIFIER_VALUE_FIELD_NUMBER: _ClassVar[int]
    POSITIVE_INT_VALUE_FIELD_NUMBER: _ClassVar[int]
    NEGATIVE_INT_VALUE_FIELD_NUMBER: _ClassVar[int]
    DOUBLE_VALUE_FIELD_NUMBER: _ClassVar[int]
    STRING_VALUE_FIELD_NUMBER: _ClassVar[int]
    AGGREGATE_VALUE_FIELD_NUMBER: _ClassVar[int]
    name: _containers.RepeatedCompositeFieldContainer[UninterpretedOption.NamePart]
    identifier_value: str
    positive_int_value: int
    negative_int_value: int
    double_value: float
    string_value: bytes
    aggregate_value: str
    def __init__(self, name: _Optional[_Iterable[_Union[UninterpretedOption.NamePart, _Mapping]]] = ..., identifier_value: _Optional[str] = ..., positive_int_value: _Optional[int] = ..., negative_int_value: _Optional[int] = ..., double_value: _Optional[float] = ..., string_value: _Optional[bytes] = ..., aggregate_value: _Optional[str] = ...) -> None: ...

class FieldOptions(_message.Message):
    __slots__ = ("packed", "uninterpreted_option")
    PACKED_FIELD_NUMBER: _ClassVar[int]
    UNINTERPRETED_OPTION_FIELD_NUMBER: _ClassVar[int]
    packed: bool
    uninterpreted_option: _containers.RepeatedCompositeFieldContainer[UninterpretedOption]
    def __init__(self, packed: bool = ..., uninterpreted_option: _Optional[_Iterable[_Union[UninterpretedOption, _Mapping]]] = ...) -> None: ...

class FieldDescriptorProto(_message.Message):
    __slots__ = ("name", "number", "label", "type", "type_name", "extendee", "default_value", "options", "oneof_index")
    class Type(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        TYPE_DOUBLE: _ClassVar[FieldDescriptorProto.Type]
        TYPE_FLOAT: _ClassVar[FieldDescriptorProto.Type]
        TYPE_INT64: _ClassVar[FieldDescriptorProto.Type]
        TYPE_UINT64: _ClassVar[FieldDescriptorProto.Type]
        TYPE_INT32: _ClassVar[FieldDescriptorProto.Type]
        TYPE_FIXED64: _ClassVar[FieldDescriptorProto.Type]
        TYPE_FIXED32: _ClassVar[FieldDescriptorProto.Type]
        TYPE_BOOL: _ClassVar[FieldDescriptorProto.Type]
        TYPE_STRING: _ClassVar[FieldDescriptorProto.Type]
        TYPE_GROUP: _ClassVar[FieldDescriptorProto.Type]
        TYPE_MESSAGE: _ClassVar[FieldDescriptorProto.Type]
        TYPE_BYTES: _ClassVar[FieldDescriptorProto.Type]
        TYPE_UINT32: _ClassVar[FieldDescriptorProto.Type]
        TYPE_ENUM: _ClassVar[FieldDescriptorProto.Type]
        TYPE_SFIXED32: _ClassVar[FieldDescriptorProto.Type]
        TYPE_SFIXED64: _ClassVar[FieldDescriptorProto.Type]
        TYPE_SINT32: _ClassVar[FieldDescriptorProto.Type]
        TYPE_SINT64: _ClassVar[FieldDescriptorProto.Type]
    TYPE_DOUBLE: FieldDescriptorProto.Type
    TYPE_FLOAT: FieldDescriptorProto.Type
    TYPE_INT64: FieldDescriptorProto.Type
    TYPE_UINT64: FieldDescriptorProto.Type
    TYPE_INT32: FieldDescriptorProto.Type
    TYPE_FIXED64: FieldDescriptorProto.Type
    TYPE_FIXED32: FieldDescriptorProto.Type
    TYPE_BOOL: FieldDescriptorProto.Type
    TYPE_STRING: FieldDescriptorProto.Type
    TYPE_GROUP: FieldDescriptorProto.Type
    TYPE_MESSAGE: FieldDescriptorProto.Type
    TYPE_BYTES: FieldDescriptorProto.Type
    TYPE_UINT32: FieldDescriptorProto.Type
    TYPE_ENUM: FieldDescriptorProto.Type
    TYPE_SFIXED32: FieldDescriptorProto.Type
    TYPE_SFIXED64: FieldDescriptorProto.Type
    TYPE_SINT32: FieldDescriptorProto.Type
    TYPE_SINT64: FieldDescriptorProto.Type
    class Label(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        LABEL_OPTIONAL: _ClassVar[FieldDescriptorProto.Label]
        LABEL_REQUIRED: _ClassVar[FieldDescriptorProto.Label]
        LABEL_REPEATED: _ClassVar[FieldDescriptorProto.Label]
    LABEL_OPTIONAL: FieldDescriptorProto.Label
    LABEL_REQUIRED: FieldDescriptorProto.Label
    LABEL_REPEATED: FieldDescriptorProto.Label
    NAME_FIELD_NUMBER: _ClassVar[int]
    NUMBER_FIELD_NUMBER: _ClassVar[int]
    LABEL_FIELD_NUMBER: _ClassVar[int]
    TYPE_FIELD_NUMBER: _ClassVar[int]
    TYPE_NAME_FIELD_NUMBER: _ClassVar[int]
    EXTENDEE_FIELD_NUMBER: _ClassVar[int]
    DEFAULT_VALUE_FIELD_NUMBER: _ClassVar[int]
    OPTIONS_FIELD_NUMBER: _ClassVar[int]
    ONEOF_INDEX_FIELD_NUMBER: _ClassVar[int]
    name: str
    number: int
    label: FieldDescriptorProto.Label
    type: FieldDescriptorProto.Type
    type_name: str
    extendee: str
    default_value: str
    options: FieldOptions
    oneof_index: int
    def __init__(self, name: _Optional[str] = ..., number: _Optional[int] = ..., label: _Optional[_Union[FieldDescriptorProto.Label, str]] = ..., type: _Optional[_Union[FieldDescriptorProto.Type, str]] = ..., type_name: _Optional[str] = ..., extendee: _Optional[str] = ..., default_value: _Optional[str] = ..., options: _Optional[_Union[FieldOptions, _Mapping]] = ..., oneof_index: _Optional[int] = ...) -> None: ...

class OneofDescriptorProto(_message.Message):
    __slots__ = ("name", "options")
    NAME_FIELD_NUMBER: _ClassVar[int]
    OPTIONS_FIELD_NUMBER: _ClassVar[int]
    name: str
    options: OneofOptions
    def __init__(self, name: _Optional[str] = ..., options: _Optional[_Union[OneofOptions, _Mapping]] = ...) -> None: ...

class EnumDescriptorProto(_message.Message):
    __slots__ = ("name", "value", "reserved_name")
    NAME_FIELD_NUMBER: _ClassVar[int]
    VALUE_FIELD_NUMBER: _ClassVar[int]
    RESERVED_NAME_FIELD_NUMBER: _ClassVar[int]
    name: str
    value: _containers.RepeatedCompositeFieldContainer[EnumValueDescriptorProto]
    reserved_name: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, name: _Optional[str] = ..., value: _Optional[_Iterable[_Union[EnumValueDescriptorProto, _Mapping]]] = ..., reserved_name: _Optional[_Iterable[str]] = ...) -> None: ...

class EnumValueDescriptorProto(_message.Message):
    __slots__ = ("name", "number")
    NAME_FIELD_NUMBER: _ClassVar[int]
    NUMBER_FIELD_NUMBER: _ClassVar[int]
    name: str
    number: int
    def __init__(self, name: _Optional[str] = ..., number: _Optional[int] = ...) -> None: ...

class OneofOptions(_message.Message):
    __slots__ = ()
    Extensions: _python_message._ExtensionDict
    def __init__(self) -> None: ...
