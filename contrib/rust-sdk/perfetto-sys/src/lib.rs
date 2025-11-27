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

//! # Safety
//!
//! This crate provides raw FFI bindings to **libperfetto_c**.
//! - All `extern "C"` functions are `unsafe` to call. Callers must uphold the
//!   preconditions documented by the upstream C headers/manual.
//! - Pointers must obey C rules: correct lifetimes, alignment, nullability,
//!   and initialization. Opaque handles follow the library’s ownership rules.
//! - Unless explicitly noted, functions are **not** thread-safe, **not**
//!   async-signal-safe, and may not be reentrant.
//! - Struct layouts with `#[repr(C)]` mirror the C ABI of the linked version.
//!   Using headers and a different linked binary may cause UB.
//! - This crate does not validate string encodings or lengths; pass NUL-terminated
//!   buffers as required by the C API.
//!
//! Prefer the higher-level safe wrapper crate `perfetto`.

#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Once;

    static INIT_TEST_ENVIRONMENT: Once = Once::new();

    fn setup_test_environment() {
        INIT_TEST_ENVIRONMENT.call_once(|| unsafe {
            let backend_args = PerfettoProducerBackendInitArgsCreate();
            PerfettoProducerSystemInit(backend_args);
            PerfettoProducerBackendInitArgsDestroy(backend_args);
        });
    }

    #[test]
    fn track_event_init() {
        setup_test_environment();
        unsafe {
            PerfettoTeInit();
            #[allow(static_mut_refs)]
            let te_process_track_uuid = perfetto_te_process_track_uuid;
            assert_ne!(te_process_track_uuid, 0);
        }
    }

    #[test]
    fn data_source_init() {
        setup_test_environment();
        let mut enabled: *mut bool = std::ptr::null_mut();
        let success = unsafe {
            let ds = PerfettoDsImplCreate();
            PerfettoDsImplRegister(ds, &raw mut enabled, std::ptr::null_mut(), 0)
        };
        assert!(success);
        assert_ne!(enabled, std::ptr::null_mut());
    }
}
