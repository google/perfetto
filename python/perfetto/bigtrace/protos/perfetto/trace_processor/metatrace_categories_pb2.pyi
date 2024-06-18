from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from typing import ClassVar as _ClassVar

DESCRIPTOR: _descriptor.FileDescriptor

class MetatraceCategories(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    QUERY_TIMELINE: _ClassVar[MetatraceCategories]
    QUERY_DETAILED: _ClassVar[MetatraceCategories]
    FUNCTION_CALL: _ClassVar[MetatraceCategories]
    DB: _ClassVar[MetatraceCategories]
    API_TIMELINE: _ClassVar[MetatraceCategories]
    NONE: _ClassVar[MetatraceCategories]
    ALL: _ClassVar[MetatraceCategories]
QUERY_TIMELINE: MetatraceCategories
QUERY_DETAILED: MetatraceCategories
FUNCTION_CALL: MetatraceCategories
DB: MetatraceCategories
API_TIMELINE: MetatraceCategories
NONE: MetatraceCategories
ALL: MetatraceCategories
