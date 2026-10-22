// Copyright (C) 2025 Rivos Inc.
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

// Manually generated with bindings for a limited set of DataSourceConfig
// fields to limit core proto bindings.

use crate::pb_enum;
use crate::pb_msg;
use crate::protos::config::test_config::*;
use crate::protos::config::track_event::track_event_config::*;

pb_enum!(DataSourceConfigSessionInitiator {
    SESSION_INITIATOR_UNSPECIFIED: 0,
    SESSION_INITIATOR_TRUSTED_SYSTEM: 1,
});

pb_enum!(DataSourceConfigBufferExhaustedPolicy {
    BUFFER_EXHAUSTED_UNSPECIFIED: 0,
    BUFFER_EXHAUSTED_DROP: 1,
    BUFFER_EXHAUSTED_STALL_THEN_ABORT: 2,
    BUFFER_EXHAUSTED_STALL_THEN_DROP: 3,
});

// `DataSourceConfig.ExperimentalTracingV2Config.ChunkSizeOption`: a chunk
// size the producer may pick for its ring buffer.
pb_msg!(DataSourceConfigExperimentalTracingV2ConfigChunkSizeOption {
    size_bytes: u32, primitive, 1,
    weight: u32, primitive, 2,
});

// `DataSourceConfig.ExperimentalTracingV2Config`: whether an instance takes
// a tracing v2 writer, where the data source supports one, and how its
// ring buffer is laid out.
pb_msg!(DataSourceConfigExperimentalTracingV2Config {
    use_v2_probability_percent: u32, primitive, 1,
    chunk_size_options: DataSourceConfigExperimentalTracingV2ConfigChunkSizeOption, msg, 2,
});

pb_msg!(DataSourceConfig {
    name: String, primitive, 1,
    target_buffer: u32, primitive, 2,
    trace_duration_ms: u32, primitive, 3,
    prefer_suspend_clock_for_duration: bool, primitive, 122,
    stop_timeout_ms: u32, primitive, 7,
    enable_extra_guardrails: bool, primitive, 6,
    session_initiator: DataSourceConfigSessionInitiator, enum, 8,
    tracing_session_id: u64, primitive, 4,
    buffer_exhausted_policy: DataSourceConfigBufferExhaustedPolicy, enum, 9,
    supports_tracing_v2: bool, primitive, 147,
    experimental_tracing_v2: DataSourceConfigExperimentalTracingV2Config, msg, 148,
    track_event_config: TrackEventConfig, msg, 113,
    for_testing: TestConfig, msg, 1001,
});
