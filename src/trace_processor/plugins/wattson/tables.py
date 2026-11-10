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

WATTSON_EXTERNAL_DEVICE_INFO_TABLE = Table(
    python_module=__file__,
    class_name="WattsonExternalDeviceInfoTable",
    sql_name="__intrinsic_wattson_external_device_info",
    columns=[
        C("device_name", CppString(), cpp_access=CppAccess.READ),
        C("use_devfreq", CppInt32(), cpp_access=CppAccess.READ),
        C("gpu_id", CppOptional(CppInt32()), cpp_access=CppAccess.READ),
        C("model_version", CppOptional(CppInt32()), cpp_access=CppAccess.READ),
        C("model_source", CppOptional(CppInt32()), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="Metadata for dynamically imported Wattson SoC power model.",
        group="Wattson",
        columns={
            "device_name": "Canonical SoC/device name.",
            "use_devfreq": "1 if devfreq is required, 0 otherwise.",
            "gpu_id": "GPU hardware ID.",
            "model_version": "Power model revision number.",
            "model_source": "Partition from which the power model was loaded.",
        },
    ),
)

WATTSON_EXTERNAL_CPU_CLUSTER_TABLE = Table(
    python_module=__file__,
    class_name="WattsonExternalCpuClusterTable",
    sql_name="__intrinsic_wattson_external_cpu_cluster",
    columns=[
        C("cpu", CppInt32(), cpp_access=CppAccess.READ),
        C("cluster", CppInt32(), cpp_access=CppAccess.READ),
        C("deep_idle_offset_ns", CppInt64(), cpp_access=CppAccess.READ),
        C("vote_by_freq", CppInt32(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="CPU topology and per-core cluster attributes for imported model.",
        group="Wattson",
        columns={
            "cpu": "CPU core index.",
            "cluster": "CPU cluster / cpufreq policy ID.",
            "deep_idle_offset_ns": "Deep idle offset in nanoseconds.",
            "vote_by_freq": "1 if 2D dependency votes by frequency, else 0.",
        },
    ),
)

WATTSON_EXTERNAL_IDLE_STATE_MAP_TABLE = Table(
    python_module=__file__,
    class_name="WattsonExternalIdleStateMapTable",
    sql_name="__intrinsic_wattson_external_idle_state_map",
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

WATTSON_EXTERNAL_CURVES_CPU_1D_TABLE = Table(
    python_module=__file__,
    class_name="WattsonExternalCurvesCpu1DTable",
    sql_name="__intrinsic_wattson_external_curves_cpu_1d",
    columns=[
        C("cluster", CppInt32(), cpp_access=CppAccess.READ),
        C("freq_khz", CppInt64(), cpp_access=CppAccess.READ),
        C("static_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("active", CppDouble(), cpp_access=CppAccess.READ),
        C("idle0", CppDouble(), cpp_access=CppAccess.READ),
        C("idle1", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="CPU 1D empirical power curves for dynamically imported model.",
        group="Wattson",
        columns={
            "cluster": "CPU cluster / cpufreq policy ID.",
            "freq_khz": "Frequency in kHz.",
            "static_mw": "Static power in mW.",
            "active": "Active power in mW.",
            "idle0": "Idle state 0 power in mW.",
            "idle1": "Idle state 1 power in mW.",
        },
    ),
)

WATTSON_EXTERNAL_CURVES_CPU_2D_TABLE = Table(
    python_module=__file__,
    class_name="WattsonExternalCurvesCpu2DTable",
    sql_name="__intrinsic_wattson_external_curves_cpu_2d",
    columns=[
        C("cluster", CppInt32(), cpp_access=CppAccess.READ),
        C("freq_khz", CppInt64(), cpp_access=CppAccess.READ),
        C("dep_cluster", CppInt32(), cpp_access=CppAccess.READ),
        C("dep_freq", CppInt64(), cpp_access=CppAccess.READ),
        C("static_mw", CppDouble(), cpp_access=CppAccess.READ),
        C("active", CppDouble(), cpp_access=CppAccess.READ),
        C("idle0", CppDouble(), cpp_access=CppAccess.READ),
        C("idle1", CppDouble(), cpp_access=CppAccess.READ),
        C("interconnect", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="CPU 2D empirical power curves for dynamically imported model.",
        group="Wattson",
        columns={
            "cluster": "CPU cluster / cpufreq policy ID.",
            "freq_khz": "Frequency in kHz.",
            "dep_cluster": "Dependent cluster / cpufreq policy ID.",
            "dep_freq": "Dependent frequency in kHz.",
            "static_mw": "Static power in mW.",
            "active": "Active power in mW.",
            "idle0": "Idle state 0 power in mW.",
            "idle1": "Idle state 1 power in mW.",
            "interconnect": "Interconnect power in mW.",
        },
    ),
)

WATTSON_EXTERNAL_CURVES_L3_TABLE = Table(
    python_module=__file__,
    class_name="WattsonExternalCurvesL3Table",
    sql_name="__intrinsic_wattson_external_curves_l3",
    columns=[
        C("freq_khz", CppInt64(), cpp_access=CppAccess.READ),
        C("dep_cluster", CppInt32(), cpp_access=CppAccess.READ),
        C("dep_freq", CppInt64(), cpp_access=CppAccess.READ),
        C("l3_hit", CppDouble(), cpp_access=CppAccess.READ),
        C("l3_miss", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="L3 cache empirical energy curves.",
        group="Wattson",
        columns={
            "freq_khz": "Frequency in kHz.",
            "dep_cluster": "Dependent cluster / cpufreq policy ID.",
            "dep_freq": "Dependent frequency.",
            "l3_hit": "L3 hit energy coefficient in nJ per access.",
            "l3_miss": "L3 miss energy coefficient in nJ per access.",
        },
    ),
)

WATTSON_EXTERNAL_CURVES_GPU_TABLE = Table(
    python_module=__file__,
    class_name="WattsonExternalCurvesGpuTable",
    sql_name="__intrinsic_wattson_external_curves_gpu",
    columns=[
        C("freq_khz", CppInt64(), cpp_access=CppAccess.READ),
        C("active", CppDouble(), cpp_access=CppAccess.READ),
        C("idle1", CppDouble(), cpp_access=CppAccess.READ),
        C("idle2", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="GPU empirical power curves.",
        group="Wattson",
        columns={
            "freq_khz": "Frequency in kHz.",
            "active": "Active power in mW.",
            "idle1": "Idle state 1 power in mW.",
            "idle2": "Idle state 2 power in mW.",
        },
    ),
)

WATTSON_EXTERNAL_CURVES_TPU_TABLE = Table(
    python_module=__file__,
    class_name="WattsonExternalCurvesTpuTable",
    sql_name="__intrinsic_wattson_external_curves_tpu",
    columns=[
        C("cluster", CppInt32(), cpp_access=CppAccess.READ),
        C("requests", CppInt32(), cpp_access=CppAccess.READ),
        C("freq", CppInt64(), cpp_access=CppAccess.READ),
        C("active", CppDouble(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc="TPU empirical power curves.",
        group="Wattson",
        columns={
            "cluster": "Cluster index.",
            "requests": "Request count.",
            "freq": "Frequency.",
            "active": "Active power in mW.",
        },
    ),
)

# Keep this list sorted.
ALL_TABLES = [
    WATTSON_EXTERNAL_CPU_CLUSTER_TABLE,
    WATTSON_EXTERNAL_CURVES_CPU_1D_TABLE,
    WATTSON_EXTERNAL_CURVES_CPU_2D_TABLE,
    WATTSON_EXTERNAL_CURVES_GPU_TABLE,
    WATTSON_EXTERNAL_CURVES_L3_TABLE,
    WATTSON_EXTERNAL_CURVES_TPU_TABLE,
    WATTSON_EXTERNAL_DEVICE_INFO_TABLE,
    WATTSON_EXTERNAL_IDLE_STATE_MAP_TABLE,
]
