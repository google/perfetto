from perfetto.bigtrace.protos.perfetto.trace_processor import trace_processor_pb2 as _trace_processor_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Iterable as _Iterable, Mapping as _Mapping, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class BigtraceQueryArgs(_message.Message):
    __slots__ = ("traces", "sql_query")
    TRACES_FIELD_NUMBER: _ClassVar[int]
    SQL_QUERY_FIELD_NUMBER: _ClassVar[int]
    traces: _containers.RepeatedScalarFieldContainer[str]
    sql_query: str
    def __init__(self, traces: _Optional[_Iterable[str]] = ..., sql_query: _Optional[str] = ...) -> None: ...

class BigtraceQueryResponse(_message.Message):
    __slots__ = ("trace", "result")
    TRACE_FIELD_NUMBER: _ClassVar[int]
    RESULT_FIELD_NUMBER: _ClassVar[int]
    trace: str
    result: _containers.RepeatedCompositeFieldContainer[_trace_processor_pb2.QueryResult]
    def __init__(self, trace: _Optional[str] = ..., result: _Optional[_Iterable[_Union[_trace_processor_pb2.QueryResult, _Mapping]]] = ...) -> None: ...
