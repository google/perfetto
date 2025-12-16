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

//! # perfetto
//!
//! This crate provides Rust bindings for Perfetto.
//!
//! It is uses the public ABI under the hood and has been designed for safe
//! and efficient usage in Rust projects. Performance critical operations
//! such as checking if a track event category is enabled is done in Rust
//! code as well as encoding of proto messages.

#![deny(missing_docs)]
#![warn(clippy::undocumented_unsafe_blocks)]
#![cfg_attr(
    feature = "intrinsics",
    allow(internal_features),
    feature(core_intrinsics)
)]

/// Data source module.
pub mod data_source;

/// Heap buffer module.
pub mod heap_buffer;

/// Protobuf decoder module.
pub mod pb_decoder;

/// Protobuf message module.
pub mod pb_msg;

/// Protobuf utils module.
pub mod pb_utils;

/// Producer module.
pub mod producer;

/// Protobuf bindings module.
pub mod protos;

/// Stream writer module.
pub mod stream_writer;

/// Tracing session module.
pub mod tracing_session;

/// Track event module.
pub mod track_event;

// FNV-1a 64-bit constants
const FNV64_OFFSET: u64 = 0xcbf29ce484222325;
const FNV64_PRIME: u64 = 0x00000100000001B3;

/// Computes the FNV-1a hash of `bytes`.
pub const fn fnv1a(bytes: &[u8]) -> u64 {
    let mut hash = FNV64_OFFSET;
    let mut i = 0;
    while i < bytes.len() {
        hash ^= bytes[i] as u64;
        hash = hash.wrapping_mul(FNV64_PRIME);
        i += 1;
    }
    hash
}

/// Helper macro that use `likely` intrinsic branch prediction hint.
#[cfg(feature = "intrinsics")]
#[doc(hidden)]
#[macro_export]
macro_rules! __likely {
    ($e:expr) => {{ std::intrinsics::likely($e) }};
}

/// Helper macro that ignores branch prediction hint.
#[cfg(not(feature = "intrinsics"))]
#[doc(hidden)]
#[macro_export]
macro_rules! __likely {
    ($e:expr) => {{ $e }};
}

/// Helper macro that use `unlikely` intrinsic branch prediction hint.
#[cfg(feature = "intrinsics")]
#[doc(hidden)]
#[macro_export]
macro_rules! __unlikely {
    ($e:expr) => {{ std::intrinsics::unlikely($e) }};
}

/// Helper macro that ignores branch prediction hint.
#[cfg(not(feature = "intrinsics"))]
#[doc(hidden)]
#[macro_export]
macro_rules! __unlikely {
    ($e:expr) => {{ $e }};
}

/// Internal utility function that converts `Box<T>` to `*mut T`.
#[doc(hidden)]
pub fn __box_as_mut_ptr<T: ?Sized>(b: &mut Box<T>) -> *mut T {
    // TODO(reveman): Use Box::as_mut_ptr() instead when stable.
    &raw mut **b
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::producer::{Backends, Producer, ProducerInitArgsBuilder};
    use crate::tracing_session::*;
    use std::sync::{Mutex, MutexGuard, Once};

    static INIT_TEST_ENVIRONMENT: Once = Once::new();
    static TEST_ENVIRONMENT_MUTEX: Mutex<()> = Mutex::new(());

    pub(crate) const PRODUCER_SHMEM_SIZE_HINT_KB: u32 = 64;

    // Perfetto uses global state internally that cannot be uninitialized so the
    // test environment and registered data sources must also be global.
    pub(crate) fn acquire_test_environment() -> MutexGuard<'static, ()> {
        INIT_TEST_ENVIRONMENT.call_once(|| {
            let producer_args = ProducerInitArgsBuilder::new()
                .backends(Backends::IN_PROCESS)
                .shmem_size_hint_kb(PRODUCER_SHMEM_SIZE_HINT_KB);
            Producer::init(producer_args.build());
        });
        TEST_ENVIRONMENT_MUTEX.lock().unwrap()
    }

    #[derive(Default)]
    #[must_use = "This is a builder; remember to call `.build()` (or keep chaining)."]
    pub(crate) struct TracingSessionBuilder {
        data_source_name: String,
        enabled_categories: Vec<String>,
        disabled_categories: Vec<String>,
    }

    impl TracingSessionBuilder {
        pub fn new() -> Self {
            Self::default()
        }

        #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
        pub fn set_data_source_name(mut self, name: impl Into<String>) -> Self {
            self.data_source_name = name.into();
            self
        }

        #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
        pub fn add_enabled_category(mut self, category: impl Into<String>) -> Self {
            self.enabled_categories.push(category.into());
            self
        }

        #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
        pub fn add_disabled_category(mut self, category: impl Into<String>) -> Self {
            self.disabled_categories.push(category.into());
            self
        }

        fn build_proto_config(&self) -> Vec<u8> {
            use crate::{
                heap_buffer::HeapBuffer,
                pb_msg::{PbMsg, PbMsgWriter},
                protos::config::{
                    data_source_config::DataSourceConfig,
                    trace_config::{BufferConfig, DataSource, TraceConfig},
                    track_event::track_event_config::TrackEventConfig,
                },
            };
            let writer = PbMsgWriter::new();
            let hb = HeapBuffer::new(&writer.writer);
            let mut msg = PbMsg::new(&writer).unwrap();
            {
                let mut cfg = TraceConfig { msg: &mut msg };
                cfg.set_buffers(|buf_cfg: &mut BufferConfig| {
                    buf_cfg.set_size_kb(1024);
                });
                cfg.set_data_sources(|data_sources: &mut DataSource| {
                    data_sources.set_config(|ds_cfg: &mut DataSourceConfig| {
                        ds_cfg.set_name(&self.data_source_name);
                        if !self.enabled_categories.is_empty()
                            || !self.disabled_categories.is_empty()
                        {
                            ds_cfg.set_track_event_config(|te_cfg: &mut TrackEventConfig| {
                                for enabled_catagory in &self.enabled_categories {
                                    te_cfg.set_enabled_categories(enabled_catagory);
                                }
                                for disabled_catagory in &self.disabled_categories {
                                    te_cfg.set_disabled_categories(disabled_catagory);
                                }
                            });
                        }
                    });
                });
            }
            msg.finalize();
            let cfg_size = writer.writer.get_written_size();
            let mut cfg_buffer: Vec<u8> = vec![0u8; cfg_size];
            hb.copy_into(&mut cfg_buffer);

            cfg_buffer
        }

        pub fn build(&self) -> Result<TracingSession, TracingSessionError> {
            let config = self.build_proto_config();
            let mut ts = TracingSession::in_process()?;
            ts.setup(&config);
            Ok(ts)
        }
    }

    #[test]
    fn fnv1a_hash() {
        assert_eq!(fnv1a("mytrack".as_bytes()), 9332035348890697650);
    }

    #[test]
    fn unlikely_conditional() {
        if __unlikely!(fnv1a("mystring".as_bytes()) == 0) {
            unreachable!();
        }
    }
}
