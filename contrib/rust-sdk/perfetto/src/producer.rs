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

use bitflags::bitflags;
use perfetto_sdk_sys::*;
use std::{ffi::CString, ptr, time::Duration};
use thiserror::Error;

/// Producer errors.
#[derive(Error, Debug, PartialEq)]
pub enum ProducerError {
    /// Invalid C string.
    #[error("Invalid string: {0}.")]
    InvalidString(std::ffi::NulError),
    /// Invalid TTL value.
    #[error("Invalid TTL: {0}.")]
    InvalidTTL(std::num::TryFromIntError),
}

bitflags! {
    /// Producer backend flags.
    #[derive(Default, Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub struct Backends: u32 {
        /// The in-process tracing backend. Keeps trace buffers in the process memory.
        const IN_PROCESS = 0b00000001;
        /// The system tracing backend. Connects to the system tracing service (e.g.
        /// on Linux/Android/Mac uses a named UNIX socket).
        const SYSTEM = 0b00000010;
    }
}

/// Producer arguments struct.
#[derive(Default)]
pub struct ProducerInitArgs {
    backends: Backends,
    shmem_size_hint_kb: u32,
}

/// Producer arguments builder.
#[derive(Default)]
#[must_use = "This is a builder; remember to call `.build()` (or keep chaining)."]
pub struct ProducerInitArgsBuilder {
    args: ProducerInitArgs,
}

impl ProducerInitArgsBuilder {
    /// Create new producer arguments builder.
    pub fn new() -> Self {
        Self::default()
    }

    /// Set backends, or-combination of one or more of the above `Backends` flags.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn backends(mut self, backends: Backends) -> Self {
        self.args.backends = backends;
        self
    }

    /// Tunes the size of the shared memory buffer between the current
    /// process and the service backend(s). This is a trade-off between memory
    /// footprint and the ability to sustain bursts of trace writes.
    /// If set, the value must be a multiple of 4KB. The value can be ignored if
    /// larger than kMaxShmSize (32MB) or not a multiple of 4KB.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn shmem_size_hint_kb(mut self, shmem_size_hint_kb: u32) -> Self {
        self.args.shmem_size_hint_kb = shmem_size_hint_kb;
        self
    }

    /// Returns producer arguments struct.
    pub fn build(&self) -> &ProducerInitArgs {
        &self.args
    }
}

/// Opaque struct to an object that stores the initialization params.
pub struct Producer {}

impl Producer {
    /// Initializes the global perfetto producer.
    ///
    /// It's ok to call this function multiple times, but if a backend was already
    /// initialized, most of `args` would be ignored.
    pub fn init(args: &ProducerInitArgs) {
        // SAFETY: FFI call with no outstanding preconditions.
        let backend_args = unsafe { PerfettoProducerBackendInitArgsCreate() };
        // SAFETY: `backend_args` must have been created using
        // PerfettoProducerBackendInitArgsCreate.
        unsafe {
            PerfettoProducerBackendInitArgsSetShmemSizeHintKb(
                backend_args,
                args.shmem_size_hint_kb,
            );
            if args.backends.contains(Backends::IN_PROCESS) {
                PerfettoProducerInProcessInit(backend_args);
            }
            if args.backends.contains(Backends::SYSTEM) {
                PerfettoProducerSystemInit(backend_args);
            }
        }
        // SAFETY: `backend_args` must have been created using
        // PerfettoProducerBackendInitArgsCreate.
        unsafe { PerfettoProducerBackendInitArgsDestroy(backend_args) };
    }

    /// Informs the tracing services to activate the single trigger `trigger_name` if
    /// any tracing session was waiting for it.
    ///
    /// Sends the trigger signal to all the initialized backends that are currently
    /// connected and that connect in the next `ttl_ms` milliseconds (but
    /// returns immediately anyway).
    pub fn activate_trigger(trigger_name: &str, ttl: Duration) -> Result<(), ProducerError> {
        let ctrigger_name = CString::new(trigger_name).map_err(ProducerError::InvalidString)?;
        let mut trigger_names = [ctrigger_name.as_ptr(), ptr::null_mut()];
        let ttl_ms = ttl
            .as_millis()
            .try_into()
            .map_err(ProducerError::InvalidTTL)?;
        // SAFETY: `trigger_names` must be a null terminated array of C strings.
        unsafe { PerfettoProducerActivateTriggers(trigger_names.as_mut_ptr(), ttl_ms) };
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tests::acquire_test_environment;
    use std::error::Error;

    #[test]
    fn activate_trigger() -> Result<(), Box<dyn Error>> {
        let _lock = acquire_test_environment();
        Producer::activate_trigger("trigger_name", Duration::from_millis(10))?;
        Ok(())
    }
}
