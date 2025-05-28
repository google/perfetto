from google.protobuf.internal import containers as _containers
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Iterable as _Iterable, Mapping as _Mapping, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class PerfettoSqlStructuredQuery(_message.Message):
    __slots__ = ("id", "table", "sql", "simple_slices", "inner_query", "inner_query_id", "interval_intersect", "filters", "group_by", "select_columns")
    class Table(_message.Message):
        __slots__ = ("table_name", "module_name", "column_names")
        TABLE_NAME_FIELD_NUMBER: _ClassVar[int]
        MODULE_NAME_FIELD_NUMBER: _ClassVar[int]
        COLUMN_NAMES_FIELD_NUMBER: _ClassVar[int]
        table_name: str
        module_name: str
        column_names: _containers.RepeatedScalarFieldContainer[str]
        def __init__(self, table_name: _Optional[str] = ..., module_name: _Optional[str] = ..., column_names: _Optional[_Iterable[str]] = ...) -> None: ...
    class SimpleSlices(_message.Message):
        __slots__ = ("slice_name_glob", "thread_name_glob", "process_name_glob", "track_name_glob")
        SLICE_NAME_GLOB_FIELD_NUMBER: _ClassVar[int]
        THREAD_NAME_GLOB_FIELD_NUMBER: _ClassVar[int]
        PROCESS_NAME_GLOB_FIELD_NUMBER: _ClassVar[int]
        TRACK_NAME_GLOB_FIELD_NUMBER: _ClassVar[int]
        slice_name_glob: str
        thread_name_glob: str
        process_name_glob: str
        track_name_glob: str
        def __init__(self, slice_name_glob: _Optional[str] = ..., thread_name_glob: _Optional[str] = ..., process_name_glob: _Optional[str] = ..., track_name_glob: _Optional[str] = ...) -> None: ...
    class Sql(_message.Message):
        __slots__ = ("sql", "column_names", "preamble")
        SQL_FIELD_NUMBER: _ClassVar[int]
        COLUMN_NAMES_FIELD_NUMBER: _ClassVar[int]
        PREAMBLE_FIELD_NUMBER: _ClassVar[int]
        sql: str
        column_names: _containers.RepeatedScalarFieldContainer[str]
        preamble: str
        def __init__(self, sql: _Optional[str] = ..., column_names: _Optional[_Iterable[str]] = ..., preamble: _Optional[str] = ...) -> None: ...
    class IntervalIntersect(_message.Message):
        __slots__ = ("base", "interval_intersect")
        BASE_FIELD_NUMBER: _ClassVar[int]
        INTERVAL_INTERSECT_FIELD_NUMBER: _ClassVar[int]
        base: PerfettoSqlStructuredQuery
        interval_intersect: _containers.RepeatedCompositeFieldContainer[PerfettoSqlStructuredQuery]
        def __init__(self, base: _Optional[_Union[PerfettoSqlStructuredQuery, _Mapping]] = ..., interval_intersect: _Optional[_Iterable[_Union[PerfettoSqlStructuredQuery, _Mapping]]] = ...) -> None: ...
    class Filter(_message.Message):
        __slots__ = ("column_name", "op", "string_rhs", "double_rhs", "int64_rhs")
        class Operator(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
            __slots__ = ()
            UNKNOWN: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
            EQUAL: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
            NOT_EQUAL: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
            LESS_THAN: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
            LESS_THAN_EQUAL: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
            GREATER_THAN: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
            GREATER_THAN_EQUAL: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
            IS_NULL: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
            IS_NOT_NULL: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
            GLOB: _ClassVar[PerfettoSqlStructuredQuery.Filter.Operator]
        UNKNOWN: PerfettoSqlStructuredQuery.Filter.Operator
        EQUAL: PerfettoSqlStructuredQuery.Filter.Operator
        NOT_EQUAL: PerfettoSqlStructuredQuery.Filter.Operator
        LESS_THAN: PerfettoSqlStructuredQuery.Filter.Operator
        LESS_THAN_EQUAL: PerfettoSqlStructuredQuery.Filter.Operator
        GREATER_THAN: PerfettoSqlStructuredQuery.Filter.Operator
        GREATER_THAN_EQUAL: PerfettoSqlStructuredQuery.Filter.Operator
        IS_NULL: PerfettoSqlStructuredQuery.Filter.Operator
        IS_NOT_NULL: PerfettoSqlStructuredQuery.Filter.Operator
        GLOB: PerfettoSqlStructuredQuery.Filter.Operator
        COLUMN_NAME_FIELD_NUMBER: _ClassVar[int]
        OP_FIELD_NUMBER: _ClassVar[int]
        STRING_RHS_FIELD_NUMBER: _ClassVar[int]
        DOUBLE_RHS_FIELD_NUMBER: _ClassVar[int]
        INT64_RHS_FIELD_NUMBER: _ClassVar[int]
        column_name: str
        op: PerfettoSqlStructuredQuery.Filter.Operator
        string_rhs: _containers.RepeatedScalarFieldContainer[str]
        double_rhs: _containers.RepeatedScalarFieldContainer[float]
        int64_rhs: _containers.RepeatedScalarFieldContainer[int]
        def __init__(self, column_name: _Optional[str] = ..., op: _Optional[_Union[PerfettoSqlStructuredQuery.Filter.Operator, str]] = ..., string_rhs: _Optional[_Iterable[str]] = ..., double_rhs: _Optional[_Iterable[float]] = ..., int64_rhs: _Optional[_Iterable[int]] = ...) -> None: ...
    class GroupBy(_message.Message):
        __slots__ = ("column_names", "aggregates")
        class Aggregate(_message.Message):
            __slots__ = ("column_name", "op", "result_column_name")
            class Op(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
                __slots__ = ()
                UNSPECIFIED: _ClassVar[PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op]
                COUNT: _ClassVar[PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op]
                SUM: _ClassVar[PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op]
                MIN: _ClassVar[PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op]
                MAX: _ClassVar[PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op]
                MEAN: _ClassVar[PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op]
                MEDIAN: _ClassVar[PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op]
                DURATION_WEIGHTED_MEAN: _ClassVar[PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op]
            UNSPECIFIED: PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
            COUNT: PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
            SUM: PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
            MIN: PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
            MAX: PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
            MEAN: PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
            MEDIAN: PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
            DURATION_WEIGHTED_MEAN: PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
            COLUMN_NAME_FIELD_NUMBER: _ClassVar[int]
            OP_FIELD_NUMBER: _ClassVar[int]
            RESULT_COLUMN_NAME_FIELD_NUMBER: _ClassVar[int]
            column_name: str
            op: PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
            result_column_name: str
            def __init__(self, column_name: _Optional[str] = ..., op: _Optional[_Union[PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op, str]] = ..., result_column_name: _Optional[str] = ...) -> None: ...
        COLUMN_NAMES_FIELD_NUMBER: _ClassVar[int]
        AGGREGATES_FIELD_NUMBER: _ClassVar[int]
        column_names: _containers.RepeatedScalarFieldContainer[str]
        aggregates: _containers.RepeatedCompositeFieldContainer[PerfettoSqlStructuredQuery.GroupBy.Aggregate]
        def __init__(self, column_names: _Optional[_Iterable[str]] = ..., aggregates: _Optional[_Iterable[_Union[PerfettoSqlStructuredQuery.GroupBy.Aggregate, _Mapping]]] = ...) -> None: ...
    class SelectColumn(_message.Message):
        __slots__ = ("column_name", "alias")
        COLUMN_NAME_FIELD_NUMBER: _ClassVar[int]
        ALIAS_FIELD_NUMBER: _ClassVar[int]
        column_name: str
        alias: str
        def __init__(self, column_name: _Optional[str] = ..., alias: _Optional[str] = ...) -> None: ...
    ID_FIELD_NUMBER: _ClassVar[int]
    TABLE_FIELD_NUMBER: _ClassVar[int]
    SQL_FIELD_NUMBER: _ClassVar[int]
    SIMPLE_SLICES_FIELD_NUMBER: _ClassVar[int]
    INNER_QUERY_FIELD_NUMBER: _ClassVar[int]
    INNER_QUERY_ID_FIELD_NUMBER: _ClassVar[int]
    INTERVAL_INTERSECT_FIELD_NUMBER: _ClassVar[int]
    FILTERS_FIELD_NUMBER: _ClassVar[int]
    GROUP_BY_FIELD_NUMBER: _ClassVar[int]
    SELECT_COLUMNS_FIELD_NUMBER: _ClassVar[int]
    id: str
    table: PerfettoSqlStructuredQuery.Table
    sql: PerfettoSqlStructuredQuery.Sql
    simple_slices: PerfettoSqlStructuredQuery.SimpleSlices
    inner_query: PerfettoSqlStructuredQuery
    inner_query_id: str
    interval_intersect: PerfettoSqlStructuredQuery.IntervalIntersect
    filters: _containers.RepeatedCompositeFieldContainer[PerfettoSqlStructuredQuery.Filter]
    group_by: PerfettoSqlStructuredQuery.GroupBy
    select_columns: _containers.RepeatedCompositeFieldContainer[PerfettoSqlStructuredQuery.SelectColumn]
    def __init__(self, id: _Optional[str] = ..., table: _Optional[_Union[PerfettoSqlStructuredQuery.Table, _Mapping]] = ..., sql: _Optional[_Union[PerfettoSqlStructuredQuery.Sql, _Mapping]] = ..., simple_slices: _Optional[_Union[PerfettoSqlStructuredQuery.SimpleSlices, _Mapping]] = ..., inner_query: _Optional[_Union[PerfettoSqlStructuredQuery, _Mapping]] = ..., inner_query_id: _Optional[str] = ..., interval_intersect: _Optional[_Union[PerfettoSqlStructuredQuery.IntervalIntersect, _Mapping]] = ..., filters: _Optional[_Iterable[_Union[PerfettoSqlStructuredQuery.Filter, _Mapping]]] = ..., group_by: _Optional[_Union[PerfettoSqlStructuredQuery.GroupBy, _Mapping]] = ..., select_columns: _Optional[_Iterable[_Union[PerfettoSqlStructuredQuery.SelectColumn, _Mapping]]] = ...) -> None: ...
