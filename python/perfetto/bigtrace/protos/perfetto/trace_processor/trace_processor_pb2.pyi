from perfetto.bigtrace.protos.perfetto.common import descriptor_pb2 as _descriptor_pb2
from perfetto.bigtrace.protos.perfetto.trace_processor import metatrace_categories_pb2 as _metatrace_categories_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Iterable as _Iterable, Mapping as _Mapping, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class TraceProcessorApiVersion(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    TRACE_PROCESSOR_CURRENT_API_VERSION: _ClassVar[TraceProcessorApiVersion]
TRACE_PROCESSOR_CURRENT_API_VERSION: TraceProcessorApiVersion

class TraceProcessorRpcStream(_message.Message):
    __slots__ = ("msg",)
    MSG_FIELD_NUMBER: _ClassVar[int]
    msg: _containers.RepeatedCompositeFieldContainer[TraceProcessorRpc]
    def __init__(self, msg: _Optional[_Iterable[_Union[TraceProcessorRpc, _Mapping]]] = ...) -> None: ...

class TraceProcessorRpc(_message.Message):
    __slots__ = ("seq", "fatal_error", "request", "response", "invalid_request", "append_trace_data", "query_args", "compute_metric_args", "enable_metatrace_args", "reset_trace_processor_args", "append_result", "query_result", "metric_result", "metric_descriptors", "metatrace", "status")
    class TraceProcessorMethod(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        TPM_UNSPECIFIED: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_APPEND_TRACE_DATA: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_FINALIZE_TRACE_DATA: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_QUERY_STREAMING: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_COMPUTE_METRIC: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_GET_METRIC_DESCRIPTORS: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_RESTORE_INITIAL_TABLES: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_ENABLE_METATRACE: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_DISABLE_AND_READ_METATRACE: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_GET_STATUS: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
        TPM_RESET_TRACE_PROCESSOR: _ClassVar[TraceProcessorRpc.TraceProcessorMethod]
    TPM_UNSPECIFIED: TraceProcessorRpc.TraceProcessorMethod
    TPM_APPEND_TRACE_DATA: TraceProcessorRpc.TraceProcessorMethod
    TPM_FINALIZE_TRACE_DATA: TraceProcessorRpc.TraceProcessorMethod
    TPM_QUERY_STREAMING: TraceProcessorRpc.TraceProcessorMethod
    TPM_COMPUTE_METRIC: TraceProcessorRpc.TraceProcessorMethod
    TPM_GET_METRIC_DESCRIPTORS: TraceProcessorRpc.TraceProcessorMethod
    TPM_RESTORE_INITIAL_TABLES: TraceProcessorRpc.TraceProcessorMethod
    TPM_ENABLE_METATRACE: TraceProcessorRpc.TraceProcessorMethod
    TPM_DISABLE_AND_READ_METATRACE: TraceProcessorRpc.TraceProcessorMethod
    TPM_GET_STATUS: TraceProcessorRpc.TraceProcessorMethod
    TPM_RESET_TRACE_PROCESSOR: TraceProcessorRpc.TraceProcessorMethod
    SEQ_FIELD_NUMBER: _ClassVar[int]
    FATAL_ERROR_FIELD_NUMBER: _ClassVar[int]
    REQUEST_FIELD_NUMBER: _ClassVar[int]
    RESPONSE_FIELD_NUMBER: _ClassVar[int]
    INVALID_REQUEST_FIELD_NUMBER: _ClassVar[int]
    APPEND_TRACE_DATA_FIELD_NUMBER: _ClassVar[int]
    QUERY_ARGS_FIELD_NUMBER: _ClassVar[int]
    COMPUTE_METRIC_ARGS_FIELD_NUMBER: _ClassVar[int]
    ENABLE_METATRACE_ARGS_FIELD_NUMBER: _ClassVar[int]
    RESET_TRACE_PROCESSOR_ARGS_FIELD_NUMBER: _ClassVar[int]
    APPEND_RESULT_FIELD_NUMBER: _ClassVar[int]
    QUERY_RESULT_FIELD_NUMBER: _ClassVar[int]
    METRIC_RESULT_FIELD_NUMBER: _ClassVar[int]
    METRIC_DESCRIPTORS_FIELD_NUMBER: _ClassVar[int]
    METATRACE_FIELD_NUMBER: _ClassVar[int]
    STATUS_FIELD_NUMBER: _ClassVar[int]
    seq: int
    fatal_error: str
    request: TraceProcessorRpc.TraceProcessorMethod
    response: TraceProcessorRpc.TraceProcessorMethod
    invalid_request: TraceProcessorRpc.TraceProcessorMethod
    append_trace_data: bytes
    query_args: QueryArgs
    compute_metric_args: ComputeMetricArgs
    enable_metatrace_args: EnableMetatraceArgs
    reset_trace_processor_args: ResetTraceProcessorArgs
    append_result: AppendTraceDataResult
    query_result: QueryResult
    metric_result: ComputeMetricResult
    metric_descriptors: DescriptorSet
    metatrace: DisableAndReadMetatraceResult
    status: StatusResult
    def __init__(self, seq: _Optional[int] = ..., fatal_error: _Optional[str] = ..., request: _Optional[_Union[TraceProcessorRpc.TraceProcessorMethod, str]] = ..., response: _Optional[_Union[TraceProcessorRpc.TraceProcessorMethod, str]] = ..., invalid_request: _Optional[_Union[TraceProcessorRpc.TraceProcessorMethod, str]] = ..., append_trace_data: _Optional[bytes] = ..., query_args: _Optional[_Union[QueryArgs, _Mapping]] = ..., compute_metric_args: _Optional[_Union[ComputeMetricArgs, _Mapping]] = ..., enable_metatrace_args: _Optional[_Union[EnableMetatraceArgs, _Mapping]] = ..., reset_trace_processor_args: _Optional[_Union[ResetTraceProcessorArgs, _Mapping]] = ..., append_result: _Optional[_Union[AppendTraceDataResult, _Mapping]] = ..., query_result: _Optional[_Union[QueryResult, _Mapping]] = ..., metric_result: _Optional[_Union[ComputeMetricResult, _Mapping]] = ..., metric_descriptors: _Optional[_Union[DescriptorSet, _Mapping]] = ..., metatrace: _Optional[_Union[DisableAndReadMetatraceResult, _Mapping]] = ..., status: _Optional[_Union[StatusResult, _Mapping]] = ...) -> None: ...

class AppendTraceDataResult(_message.Message):
    __slots__ = ("total_bytes_parsed", "error")
    TOTAL_BYTES_PARSED_FIELD_NUMBER: _ClassVar[int]
    ERROR_FIELD_NUMBER: _ClassVar[int]
    total_bytes_parsed: int
    error: str
    def __init__(self, total_bytes_parsed: _Optional[int] = ..., error: _Optional[str] = ...) -> None: ...

class QueryArgs(_message.Message):
    __slots__ = ("sql_query", "tag")
    SQL_QUERY_FIELD_NUMBER: _ClassVar[int]
    TAG_FIELD_NUMBER: _ClassVar[int]
    sql_query: str
    tag: str
    def __init__(self, sql_query: _Optional[str] = ..., tag: _Optional[str] = ...) -> None: ...

class QueryResult(_message.Message):
    __slots__ = ("column_names", "error", "batch", "statement_count", "statement_with_output_count", "last_statement_sql")
    class CellsBatch(_message.Message):
        __slots__ = ("cells", "varint_cells", "float64_cells", "blob_cells", "string_cells", "is_last_batch")
        class CellType(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
            __slots__ = ()
            CELL_INVALID: _ClassVar[QueryResult.CellsBatch.CellType]
            CELL_NULL: _ClassVar[QueryResult.CellsBatch.CellType]
            CELL_VARINT: _ClassVar[QueryResult.CellsBatch.CellType]
            CELL_FLOAT64: _ClassVar[QueryResult.CellsBatch.CellType]
            CELL_STRING: _ClassVar[QueryResult.CellsBatch.CellType]
            CELL_BLOB: _ClassVar[QueryResult.CellsBatch.CellType]
        CELL_INVALID: QueryResult.CellsBatch.CellType
        CELL_NULL: QueryResult.CellsBatch.CellType
        CELL_VARINT: QueryResult.CellsBatch.CellType
        CELL_FLOAT64: QueryResult.CellsBatch.CellType
        CELL_STRING: QueryResult.CellsBatch.CellType
        CELL_BLOB: QueryResult.CellsBatch.CellType
        CELLS_FIELD_NUMBER: _ClassVar[int]
        VARINT_CELLS_FIELD_NUMBER: _ClassVar[int]
        FLOAT64_CELLS_FIELD_NUMBER: _ClassVar[int]
        BLOB_CELLS_FIELD_NUMBER: _ClassVar[int]
        STRING_CELLS_FIELD_NUMBER: _ClassVar[int]
        IS_LAST_BATCH_FIELD_NUMBER: _ClassVar[int]
        cells: _containers.RepeatedScalarFieldContainer[QueryResult.CellsBatch.CellType]
        varint_cells: _containers.RepeatedScalarFieldContainer[int]
        float64_cells: _containers.RepeatedScalarFieldContainer[float]
        blob_cells: _containers.RepeatedScalarFieldContainer[bytes]
        string_cells: str
        is_last_batch: bool
        def __init__(self, cells: _Optional[_Iterable[_Union[QueryResult.CellsBatch.CellType, str]]] = ..., varint_cells: _Optional[_Iterable[int]] = ..., float64_cells: _Optional[_Iterable[float]] = ..., blob_cells: _Optional[_Iterable[bytes]] = ..., string_cells: _Optional[str] = ..., is_last_batch: bool = ...) -> None: ...
    COLUMN_NAMES_FIELD_NUMBER: _ClassVar[int]
    ERROR_FIELD_NUMBER: _ClassVar[int]
    BATCH_FIELD_NUMBER: _ClassVar[int]
    STATEMENT_COUNT_FIELD_NUMBER: _ClassVar[int]
    STATEMENT_WITH_OUTPUT_COUNT_FIELD_NUMBER: _ClassVar[int]
    LAST_STATEMENT_SQL_FIELD_NUMBER: _ClassVar[int]
    column_names: _containers.RepeatedScalarFieldContainer[str]
    error: str
    batch: _containers.RepeatedCompositeFieldContainer[QueryResult.CellsBatch]
    statement_count: int
    statement_with_output_count: int
    last_statement_sql: str
    def __init__(self, column_names: _Optional[_Iterable[str]] = ..., error: _Optional[str] = ..., batch: _Optional[_Iterable[_Union[QueryResult.CellsBatch, _Mapping]]] = ..., statement_count: _Optional[int] = ..., statement_with_output_count: _Optional[int] = ..., last_statement_sql: _Optional[str] = ...) -> None: ...

class StatusArgs(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class StatusResult(_message.Message):
    __slots__ = ("loaded_trace_name", "human_readable_version", "api_version", "version_code")
    LOADED_TRACE_NAME_FIELD_NUMBER: _ClassVar[int]
    HUMAN_READABLE_VERSION_FIELD_NUMBER: _ClassVar[int]
    API_VERSION_FIELD_NUMBER: _ClassVar[int]
    VERSION_CODE_FIELD_NUMBER: _ClassVar[int]
    loaded_trace_name: str
    human_readable_version: str
    api_version: int
    version_code: str
    def __init__(self, loaded_trace_name: _Optional[str] = ..., human_readable_version: _Optional[str] = ..., api_version: _Optional[int] = ..., version_code: _Optional[str] = ...) -> None: ...

class ComputeMetricArgs(_message.Message):
    __slots__ = ("metric_names", "format")
    class ResultFormat(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        BINARY_PROTOBUF: _ClassVar[ComputeMetricArgs.ResultFormat]
        TEXTPROTO: _ClassVar[ComputeMetricArgs.ResultFormat]
        JSON: _ClassVar[ComputeMetricArgs.ResultFormat]
    BINARY_PROTOBUF: ComputeMetricArgs.ResultFormat
    TEXTPROTO: ComputeMetricArgs.ResultFormat
    JSON: ComputeMetricArgs.ResultFormat
    METRIC_NAMES_FIELD_NUMBER: _ClassVar[int]
    FORMAT_FIELD_NUMBER: _ClassVar[int]
    metric_names: _containers.RepeatedScalarFieldContainer[str]
    format: ComputeMetricArgs.ResultFormat
    def __init__(self, metric_names: _Optional[_Iterable[str]] = ..., format: _Optional[_Union[ComputeMetricArgs.ResultFormat, str]] = ...) -> None: ...

class ComputeMetricResult(_message.Message):
    __slots__ = ("metrics", "metrics_as_prototext", "metrics_as_json", "error")
    METRICS_FIELD_NUMBER: _ClassVar[int]
    METRICS_AS_PROTOTEXT_FIELD_NUMBER: _ClassVar[int]
    METRICS_AS_JSON_FIELD_NUMBER: _ClassVar[int]
    ERROR_FIELD_NUMBER: _ClassVar[int]
    metrics: bytes
    metrics_as_prototext: str
    metrics_as_json: str
    error: str
    def __init__(self, metrics: _Optional[bytes] = ..., metrics_as_prototext: _Optional[str] = ..., metrics_as_json: _Optional[str] = ..., error: _Optional[str] = ...) -> None: ...

class EnableMetatraceArgs(_message.Message):
    __slots__ = ("categories",)
    CATEGORIES_FIELD_NUMBER: _ClassVar[int]
    categories: _metatrace_categories_pb2.MetatraceCategories
    def __init__(self, categories: _Optional[_Union[_metatrace_categories_pb2.MetatraceCategories, str]] = ...) -> None: ...

class EnableMetatraceResult(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class DisableAndReadMetatraceArgs(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class DisableAndReadMetatraceResult(_message.Message):
    __slots__ = ("metatrace", "error")
    METATRACE_FIELD_NUMBER: _ClassVar[int]
    ERROR_FIELD_NUMBER: _ClassVar[int]
    metatrace: bytes
    error: str
    def __init__(self, metatrace: _Optional[bytes] = ..., error: _Optional[str] = ...) -> None: ...

class DescriptorSet(_message.Message):
    __slots__ = ("descriptors",)
    DESCRIPTORS_FIELD_NUMBER: _ClassVar[int]
    descriptors: _containers.RepeatedCompositeFieldContainer[_descriptor_pb2.DescriptorProto]
    def __init__(self, descriptors: _Optional[_Iterable[_Union[_descriptor_pb2.DescriptorProto, _Mapping]]] = ...) -> None: ...

class ResetTraceProcessorArgs(_message.Message):
    __slots__ = ("drop_track_event_data_before", "ingest_ftrace_in_raw_table", "analyze_trace_proto_content", "ftrace_drop_until_all_cpus_valid")
    class DropTrackEventDataBefore(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        NO_DROP: _ClassVar[ResetTraceProcessorArgs.DropTrackEventDataBefore]
        TRACK_EVENT_RANGE_OF_INTEREST: _ClassVar[ResetTraceProcessorArgs.DropTrackEventDataBefore]
    NO_DROP: ResetTraceProcessorArgs.DropTrackEventDataBefore
    TRACK_EVENT_RANGE_OF_INTEREST: ResetTraceProcessorArgs.DropTrackEventDataBefore
    DROP_TRACK_EVENT_DATA_BEFORE_FIELD_NUMBER: _ClassVar[int]
    INGEST_FTRACE_IN_RAW_TABLE_FIELD_NUMBER: _ClassVar[int]
    ANALYZE_TRACE_PROTO_CONTENT_FIELD_NUMBER: _ClassVar[int]
    FTRACE_DROP_UNTIL_ALL_CPUS_VALID_FIELD_NUMBER: _ClassVar[int]
    drop_track_event_data_before: ResetTraceProcessorArgs.DropTrackEventDataBefore
    ingest_ftrace_in_raw_table: bool
    analyze_trace_proto_content: bool
    ftrace_drop_until_all_cpus_valid: bool
    def __init__(self, drop_track_event_data_before: _Optional[_Union[ResetTraceProcessorArgs.DropTrackEventDataBefore, str]] = ..., ingest_ftrace_in_raw_table: bool = ..., analyze_trace_proto_content: bool = ..., ftrace_drop_until_all_cpus_valid: bool = ...) -> None: ...
