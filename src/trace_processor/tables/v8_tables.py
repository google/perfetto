# Copyright (C) 2024 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Contains tables related to the v8 (Javasrcript Engine) Datasource.

These tables are WIP, the schema is not stable and you should not rely on them
for any serious business just yet""
"""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppAccessDuration
from python.generators.trace_processor_table.public import CppInt32
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import CppUint32 as CppBool
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc

from src.trace_processor.tables.jit_tables import JIT_CODE_TABLE
from src.trace_processor.tables.profiler_tables import (
    CPU_PROFILE_STACK_SAMPLE_TABLE, STACK_PROFILE_FRAME_TABLE)

V8_ISOLATE = Table(
    python_module=__file__,
    class_name='V8IsolateTable',
    sql_name='__intrinsic_v8_isolate',
    columns=[
        C('upid', CppUint32(), cpp_access=CppAccess.READ),
        C('internal_isolate_id', CppInt32()),
        C('embedded_blob_code_start_address', CppInt64()),
        C('embedded_blob_code_size', CppInt64()),
        C('code_range_base_address', CppOptional(CppInt64())),
        C('code_range_size', CppOptional(CppInt64())),
        C('shared_code_range', CppOptional(CppBool())),
        C('embedded_blob_code_copy_start_address', CppOptional(CppInt64())),
    ],
    tabledoc=TableDoc(
        doc='Represents one Isolate instance',
        group='v8',
        columns={
            'upid':
                'Process the isolate was created in.',
            'internal_isolate_id':
                'Internal id used by the v8 engine. Unique in a process.',
            'embedded_blob_code_start_address':
                'Absolute start address of the embedded code blob.',
            'embedded_blob_code_size':
                'Size in bytes of the embedded code blob.',
            'code_range_base_address':
                'If this Isolate defines a CodeRange its base address is stored'
                ' here',
            'code_range_size':
                'If this Isolate defines a CodeRange its size is stored here',
            'shared_code_range':
                'Whether the code range for this Isolate is shared with others'
                ' in the same process. There is at max one such shared code'
                ' range per process.',
            'embedded_blob_code_copy_start_address':
                'Used when short builtin calls are enabled, where embedded'
                ' builtins are copied into the CodeRange so calls can be'
                ' nearer.',
        },
    ),
)

V8_JS_SCRIPT = Table(
    python_module=__file__,
    class_name='V8JsScriptTable',
    sql_name='__intrinsic_v8_js_script',
    columns=[
        C('v8_isolate_id', CppTableId(V8_ISOLATE)),
        C('internal_script_id', CppInt32()),
        C('script_type', CppString()),
        C('name',
          CppString(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('source', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='Represents one Javascript script',
        group='v8',
        columns={
            'v8_isolate_id': 'V8 Isolate',
            'internal_script_id': 'Script id used by the V8 engine',
            'script_type': '',
            'name': '',
            'source': 'Actual contents of the script.',
        },
    ),
)

V8_WASM_SCRIPT = Table(
    python_module=__file__,
    class_name='V8WasmScriptTable',
    sql_name='__intrinsic_v8_wasm_script',
    columns=[
        C('v8_isolate_id', CppTableId(V8_ISOLATE)),
        C('internal_script_id', CppInt32()),
        C('url', CppString()),
        C('wire_bytes_base64', CppOptional(CppString())),
        C('source', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='Represents one WASM script',
        group='v8',
        columns={
            'v8_isolate_id': 'V8 Isolate',
            'internal_script_id': 'Script id used by the V8 engine',
            'url': 'URL of the source',
            'wire_bytes_base64': 'Raw write bytes of the script',
            'source': 'Actual contents of the script.',
        },
    ),
)

V8_JS_FUNCTION = Table(
    python_module=__file__,
    class_name='V8JsFunctionTable',
    sql_name='__intrinsic_v8_js_function',
    columns=[
        C('name',
          CppString(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C(
            'v8_js_script_id',
            CppTableId(V8_JS_SCRIPT),
            cpp_access=CppAccess.READ,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C('is_toplevel', CppBool()),
        C('kind', CppString()),
        C('line',
          CppOptional(CppUint32()),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('col',
          CppOptional(CppUint32()),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
    ],
    tabledoc=TableDoc(
        doc='Represents a v8 Javascript function',
        group='v8',
        columns={
            'name':
                '',
            'v8_js_script_id':
                ColumnDoc(
                    doc='Script where the function is defined.',
                    joinable='v8_js_script.id',
                ),
            'is_toplevel':
                'Whether this function represents the top level script',
            'kind':
                'Function kind (e.g. regular function or constructor)',
            'line':
                'Line in script where function is defined. Starts at 1',
            'col':
                'Column in script where function is defined. Starts at 1',
        },
    ),
)

V8_JS_CODE = Table(
    python_module=__file__,
    class_name='V8JsCodeTable',
    sql_name='__intrinsic_v8_js_code',
    columns=[
        C('jit_code_id', CppOptional(CppTableId(JIT_CODE_TABLE))),
        C(
            'v8_js_function_id',
            CppTableId(V8_JS_FUNCTION),
            cpp_access=CppAccess.READ,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C(
            'tier',
            CppString(),
            cpp_access=CppAccess.READ,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C('bytecode_base64', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc="""
          Represents a v8 code snippet for a Javascript function. A given
          function can have multiple code snippets (e.g. for different
          compilation tiers, or as the function moves around the heap)
        """,
        group='v8',
        columns={
            'jit_code_id':
                ColumnDoc(
                    doc="""
                  Set for all tiers except IGNITION.
                    """,
                    joinable='__intrinsic_jit_code.id',
                ),
            'v8_js_function_id':
                ColumnDoc(
                    doc='JS function for this snippet.',
                    joinable='__intrinsic_v8_js_function.id',
                ),
            'tier':
                'Compilation tier',
            'bytecode_base64':
                'Set only for the IGNITION tier (base64 encoded)',
        },
    ),
)

V8_INTERNAL_CODE = Table(
    python_module=__file__,
    class_name='V8InternalCodeTable',
    sql_name='__intrinsic_v8_internal_code',
    columns=[
        C('jit_code_id', CppTableId(JIT_CODE_TABLE)),
        C('v8_isolate_id', CppTableId(V8_ISOLATE)),
        C(
            'function_name',
            CppString(),
            cpp_access=CppAccess.READ,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C('code_type', CppString()),
    ],
    tabledoc=TableDoc(
        doc="""
          Represents a v8 code snippet for a v8 internal function.
        """,
        group='v8',
        columns={
            'jit_code_id':
                ColumnDoc(
                    doc='Associated JitCode.',
                    joinable='__intrinsic_jit_code.id',
                ),
            'v8_isolate_id':
                ColumnDoc(
                    doc="""
                  V8 Isolate this code was created in.
                    """,
                    joinable='__intrinsic_v8_isolate.id'),
            'function_name':
                'Function name.',
            'code_type':
                'Type of internal function (e.g. BYTECODE_HANDLER, BUILTIN)',
        },
    ),
)

V8_WASM_CODE = Table(
    python_module=__file__,
    class_name='V8WasmCodeTable',
    sql_name='__intrinsic_v8_wasm_code',
    columns=[
        C('jit_code_id', CppTableId(JIT_CODE_TABLE)),
        C('v8_isolate_id', CppTableId(V8_ISOLATE)),
        C('v8_wasm_script_id', CppTableId(V8_WASM_SCRIPT)),
        C('function_name', CppString(), cpp_access=CppAccess.READ),
        C('tier', CppString()),
        C('code_offset_in_module', CppInt32()),
    ],
    tabledoc=TableDoc(
        doc="""
          Represents the code associated to a WASM function
        """,
        group='v8',
        columns={
            'jit_code_id':
                ColumnDoc(
                    doc='Associated JitCode.',
                    joinable='__intrinsic_jit_code.id',
                ),
            'v8_isolate_id':
                ColumnDoc(
                    doc="""
                  V8 Isolate this code was created in.
                    """,
                    joinable='__intrinsic_v8_isolate.id'),
            'v8_wasm_script_id':
                ColumnDoc(
                    doc="""
                  Script where the function is defined.
                    """,
                    joinable='v8_wasm_script.id',
                ),
            'function_name':
                'Function name.',
            'tier':
                'Compilation tier',
            'code_offset_in_module':
                """Offset into the WASM module where the function starts""",
        },
    ),
)

V8_REGEXP_CODE = Table(
    python_module=__file__,
    class_name='V8RegexpCodeTable',
    sql_name='__intrinsic_v8_regexp_code',
    columns=[
        C('jit_code_id', CppTableId(JIT_CODE_TABLE)),
        C('v8_isolate_id', CppTableId(V8_ISOLATE)),
        C(
            'pattern',
            CppString(),
            cpp_access=CppAccess.READ,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
    ],
    tabledoc=TableDoc(
        doc="""
          Represents the code associated to a regular expression
        """,
        group='v8',
        columns={
            'jit_code_id':
                ColumnDoc(
                    doc='Associated JitCode.',
                    joinable='__intrinsic_jit_code.id',
                ),
            'v8_isolate_id':
                ColumnDoc(
                    doc="""
                  V8 Isolate this code was created in.
                    """,
                    joinable='__intrinsic_v8_isolate.id'),
            'pattern':
                """The pattern the this regular expression was compiled from""",
        },
    ),
)

V8_STACK_PROFILE_FRAME = Table(
    python_module=__file__,
    class_name='V8StackProfileFrameTable',
    sql_name='__intrinsic_v8_stack_profile_frame',
    columns=[
        C('frame_id',
          CppTableId(STACK_PROFILE_FRAME_TABLE),
          cpp_access=CppAccess.READ),
        C('tier', CppOptional(CppString()), cpp_access=CppAccess.READ),
        C('is_inlined', CppOptional(CppBool()), cpp_access=CppAccess.READ),
        C('column_number', CppOptional(CppUint32()), cpp_access=CppAccess.READ),
        C('deopt_reason', CppOptional(CppString()), cpp_access=CppAccess.READ),
        C('script_id', CppOptional(CppInt32()), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc='''V8-specific attributes attached to a stack_profile_frame row.
            One row per stack_profile_frame.id that originated from V8's CPU
            profiler.''',
        group='v8',
        columns={
            'frame_id':
                ColumnDoc(
                    doc='Stack profile frame this row attaches to.',
                    joinable='__intrinsic_stack_profile_frame.id',
                ),
            'tier':
                'Compilation tier (e.g. IGNITION, SPARKPLUG, TURBOFAN).',
            'is_inlined':
                'Whether this frame represents an inlined call site.',
            'column_number':
                'Source column for the frame, paired with frame line_number.',
            'deopt_reason':
                'Function deopt reason.',
            'script_id':
                'V8 script id for DevTools source mapping.',
        },
    ),
)

V8_CPU_PROFILE_SAMPLE = Table(
    python_module=__file__,
    class_name='V8CpuProfileSampleTable',
    sql_name='__intrinsic_v8_cpu_profile_sample',
    columns=[
        C(
            'cpu_profile_stack_sample_id',
            CppTableId(CPU_PROFILE_STACK_SAMPLE_TABLE),
            cpp_access=CppAccess.READ,
        ),
        C('sample_kind', CppOptional(CppString()), cpp_access=CppAccess.READ),
        C('leaf_line', CppOptional(CppUint32()), cpp_access=CppAccess.READ),
        C('leaf_column', CppOptional(CppUint32()), cpp_access=CppAccess.READ),
        C('session_id', CppOptional(CppInt64()), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc='''V8-specific attributes attached to a cpu_profile_stack_sample
            row.''',
        group='v8',
        columns={
            'cpu_profile_stack_sample_id':
                ColumnDoc(
                    doc='cpu_profile_stack_sample this row attaches to.',
                    joinable='__intrinsic_cpu_profile_stack_sample.id',
                ),
            'sample_kind':
                'Sample origin (NORMAL, PROGRAM, GC, IDLE, OTHER).',
            'leaf_line':
                'Per-sample leaf source line.',
            'leaf_column':
                'Per-sample leaf source column.',
            'session_id':
                'V8 CPU profile session owning this sample.',
        },
    ),
)

V8_CPU_PROFILE_SESSION = Table(
    python_module=__file__,
    class_name='V8CpuProfileSessionTable',
    sql_name='__intrinsic_v8_cpu_profile_session',
    columns=[
        C('session_id', CppInt64(), cpp_access=CppAccess.READ),
        C('utid', CppUint32(), cpp_access=CppAccess.READ),
        C('source', CppOptional(CppString()), cpp_access=CppAccess.READ),
        C('start_ts', CppInt64(), cpp_access=CppAccess.READ),
        C('end_ts',
          CppOptional(CppInt64()),
          cpp_access=CppAccess.READ_AND_LOW_PERF_WRITE),
        C('start_time_us', CppOptional(CppInt64()), cpp_access=CppAccess.READ),
        C('end_time_us',
          CppOptional(CppInt64()),
          cpp_access=CppAccess.READ_AND_LOW_PERF_WRITE),
        C('start_thread_ts', CppOptional(CppInt64()),
          cpp_access=CppAccess.READ),
        C('end_thread_ts',
          CppOptional(CppInt64()),
          cpp_access=CppAccess.READ_AND_LOW_PERF_WRITE),
    ],
    tabledoc=TableDoc(
        doc='''Session-scoped metadata for a V8 CPU profile.''',
        group='v8',
        columns={
            'session_id': 'V8-internal session id (stable across START/END).',
            'utid': 'Thread the profiler ran on.',
            'source': 'Source string.',
            'start_ts': 'TraceProcessor timestamp (ns) of the session start.',
            'end_ts': 'TraceProcessor timestamp (ns) of the session end.',
            'start_time_us': 'V8 wall-clock startTime in microseconds.',
            'end_time_us': 'V8 wall-clock endTime in microseconds.',
            'start_thread_ts': 'Thread timestamp at start (ns).',
            'end_thread_ts': 'Thread timestamp at end (ns).',
        },
    ),
)

# Keep this list sorted.
ALL_TABLES = [
    V8_ISOLATE,
    V8_JS_SCRIPT,
    V8_WASM_SCRIPT,
    V8_JS_FUNCTION,
    V8_JS_CODE,
    V8_INTERNAL_CODE,
    V8_WASM_CODE,
    V8_REGEXP_CODE,
    V8_STACK_PROFILE_FRAME,
    V8_CPU_PROFILE_SAMPLE,
    V8_CPU_PROFILE_SESSION,
]
