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

// Manually generated with bindings for an extra set of DataSourceConfig
// fields.

use crate::pb_msg;
use crate::pb_msg_ext;
use crate::protos::config::gpu::gpu_counter_config::*;
use crate::protos::config::gpu::gpu_renderstages_config::*;
use crate::protos::config::gpu::vulkan_memory_config::*;

use perfetto_sdk::protos::config::data_source_config::DataSourceConfig;

pb_msg_ext!(DataSourceConfig {
    gpu_counter_config: GpuCounterConfig, msg, 108,
    vulkan_memory_config: VulkanMemoryConfig, msg, 112,
    gpu_renderstages_config: GpuRenderStagesConfig, msg, 133,
});

/// Import this to use the extra `DataSourceConfig` fields.
pub mod prelude {
    pub use super::DataSourceConfigExt;
}
