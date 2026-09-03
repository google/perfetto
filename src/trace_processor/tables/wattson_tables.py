# Copyright (C) 2026 The Android Open Source Project
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
"""Contains tables for dynamic/out-of-tree Wattson SoC power models."""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppDouble
from python.generators.trace_processor_table.public import CppInt32
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc

WATTSON_CUSTOM_DEVICE_INFO_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomDeviceInfoTable",
    sql_name="__intrinsic_wattson_custom_device_info",
    columns=[
        C("device_name", CppString(), cpp_access=CppAccess.READ),
        C("use_devfreq", CppInt32(), cpp_access=CppAccess.READ),
        C("gpu_id", CppOptional(CppInt32()), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="Metadata for dynamically imported Wattson SoC power model.",
        group="Wattson",
        columns={
            "device_name": "Canonical SoC/device name.",
            "use_devfreq": "1 if devfreq is required, 0 otherwise.",
            "gpu_id": "GPU hardware ID.",
        },
    ),
)

WATTSON_CUSTOM_CPU_POLICY_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomCpuPolicyTable",
    sql_name="__intrinsic_wattson_custom_cpu_policy",
    columns=[
        C("cpu", CppInt32(), cpp_access=CppAccess.READ),
        C("policy", CppInt32(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="CPU core to cpufreq policy mapping for dynamically imported model.",
        group="Wattson",
        columns={
            "cpu": "CPU core index.",
            "policy": "cpufreq policy index.",
        },
    ),
)

WATTSON_CUSTOM_DEEP_IDLE_OFFSET_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomDeepIdleOffsetTable",
    sql_name="__intrinsic_wattson_custom_deep_idle_offset",
    columns=[
        C("cpu", CppInt32(), cpp_access=CppAccess.READ),
        C("offset_ns", CppInt64(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="Deep idle transition latency offsets per CPU.",
        group="Wattson",
        columns={
            "cpu": "CPU core index.",
            "offset_ns": "Deep idle offset in nanoseconds.",
        },
    ),
)

WATTSON_CUSTOM_IDLE_STATE_MAP_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomIdleStateMapTable",
    sql_name="__intrinsic_wattson_custom_idle_state_map",
    columns=[
        C("nominal_idle", CppInt64(), cpp_access=CppAccess.READ),
        C("override_idle", CppInt32(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="Idle state remapping for dynamically imported model.",
        group="Wattson",
        columns={
            "nominal_idle": "Nominal idle state.",
            "override_idle": "Override idle state index.",
        },
    ),
)

WATTSON_CUSTOM_VOTE_BY_FREQ_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomVoteByFreqTable",
    sql_name="__intrinsic_wattson_custom_vote_by_freq",
    columns=[
        C("cpu", CppInt32(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="CPUs whose 2D dependency votes by frequency instead of power.",
        group="Wattson",
        columns={
            "cpu": "CPU core index.",
        },
    ),
)

WATTSON_CUSTOM_CURVES_CPU_1D_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomCurvesCpu1DTable",
    sql_name="__intrinsic_wattson_custom_curves_cpu_1d",
    columns=[
        C("policy", CppInt32(), cpp_access=CppAccess.READ),
        C("freq_khz", CppInt64(), cpp_access=CppAccess.READ),
        C("static_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("active_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("idle0_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("idle1_mw", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="CPU 1D empirical power curves for dynamically imported model.",
        group="Wattson",
        columns={
            "policy": "cpufreq policy.",
            "freq_khz": "Frequency in kHz.",
            "static_mw": "Static power in mW.",
            "active_mw": "Active power in mW.",
            "idle0_mw": "Idle state 0 power in mW.",
            "idle1_mw": "Idle state 1 power in mW.",
        },
    ),
)

WATTSON_CUSTOM_CURVES_CPU_2D_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomCurvesCpu2DTable",
    sql_name="__intrinsic_wattson_custom_curves_cpu_2d",
    columns=[
        C("policy", CppInt32(), cpp_access=CppAccess.READ),
        C("freq_khz", CppInt64(), cpp_access=CppAccess.READ),
        C("dep_policy", CppInt32(), cpp_access=CppAccess.READ),
        C("dep_freq", CppInt64(), cpp_access=CppAccess.READ),
        C("static_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("active_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("idle0_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("idle1_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("interconnect_mw", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="CPU 2D empirical power curves for dynamically imported model.",
        group="Wattson",
        columns={
            "policy": "cpufreq policy.",
            "freq_khz": "Frequency in kHz.",
            "dep_policy": "Dependent policy.",
            "dep_freq": "Dependent frequency in kHz.",
            "static_mw": "Static power in mW.",
            "active_mw": "Active power in mW.",
            "idle0_mw": "Idle state 0 power in mW.",
            "idle1_mw": "Idle state 1 power in mW.",
            "interconnect_mw": "Interconnect power in mW.",
        },
    ),
)

WATTSON_CUSTOM_CURVES_L3_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomCurvesL3Table",
    sql_name="__intrinsic_wattson_custom_curves_l3",
    columns=[
        C("freq_khz", CppInt64(), cpp_access=CppAccess.READ),
        C("dep_policy", CppInt32(), cpp_access=CppAccess.READ),
        C("dep_freq", CppInt64(), cpp_access=CppAccess.READ),
        C("l3_hit_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("l3_miss_mw", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="L3 cache empirical power curves.",
        group="Wattson",
        columns={
            "freq_khz": "Frequency in kHz.",
            "dep_policy": "Dependent policy.",
            "dep_freq": "Dependent frequency in kHz.",
            "l3_hit_mw": "L3 hit power in mW.",
            "l3_miss_mw": "L3 miss power in mW.",
        },
    ),
)

WATTSON_CUSTOM_CURVES_GPU_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomCurvesGpuTable",
    sql_name="__intrinsic_wattson_custom_curves_gpu",
    columns=[
        C("freq_khz", CppInt64(), cpp_access=CppAccess.READ),
        C("active_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("idle1_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("idle2_mw", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="GPU empirical power curves.",
        group="Wattson",
        columns={
            "freq_khz": "Frequency in kHz.",
            "active_mw": "Active power in mW.",
            "idle1_mw": "Idle state 1 power in mW.",
            "idle2_mw": "Idle state 2 power in mW.",
        },
    ),
)

WATTSON_CUSTOM_CURVES_TPU_TABLE = Table(
    python_module=__file__,
    class_name="WattsonCustomCurvesTpuTable",
    sql_name="__intrinsic_wattson_custom_curves_tpu",
    columns=[
        C("cluster", CppInt32(), cpp_access=CppAccess.READ),
        C("requests", CppInt32(), cpp_access=CppAccess.READ),
        C("freq", CppInt64(), cpp_access=CppAccess.READ),
        C("active_mw", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="TPU empirical power curves.",
        group="Wattson",
        columns={
            "cluster": "Cluster index.",
            "requests": "Request count.",
            "freq": "Frequency.",
            "active_mw": "Active power in mW.",
        },
    ),
)

ALL_TABLES = [
    WATTSON_CUSTOM_DEVICE_INFO_TABLE,
    WATTSON_CUSTOM_CPU_POLICY_TABLE,
    WATTSON_CUSTOM_DEEP_IDLE_OFFSET_TABLE,
    WATTSON_CUSTOM_IDLE_STATE_MAP_TABLE,
    WATTSON_CUSTOM_VOTE_BY_FREQ_TABLE,
    WATTSON_CUSTOM_CURVES_CPU_1D_TABLE,
    WATTSON_CUSTOM_CURVES_CPU_2D_TABLE,
    WATTSON_CUSTOM_CURVES_L3_TABLE,
    WATTSON_CUSTOM_CURVES_GPU_TABLE,
    WATTSON_CUSTOM_CURVES_TPU_TABLE,
]
