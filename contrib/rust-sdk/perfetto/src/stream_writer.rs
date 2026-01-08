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

use perfetto_sdk_sys::*;
use std::{cell::RefCell, ptr, slice};

/// A `StreamWriter` owns a chunk of memory that the user can write to.
pub struct StreamWriter {
    pub(crate) writer: RefCell<PerfettoStreamWriter>,
}

impl StreamWriter {
    /// Create new stream writer.
    pub fn new() -> Self {
        Self::default()
    }

    /// Returns the number of bytes available for writing in the current chunk.
    pub fn available_bytes(&self) -> usize {
        let writer = self.writer.borrow_mut();
        assert!(writer.end >= writer.write_ptr);

        // SAFETY: `writer.end` must be >= to `writer.write_ptr`.
        unsafe { writer.end.offset_from(writer.write_ptr) as usize }
    }

    /// Writes bytes from `src` to the writer.
    ///
    /// SAFETY: `available_bytes()` must be >= `src.len()`.
    pub fn append_bytes_unchecked(&self, src: &[u8]) {
        assert!(src.len() <= self.available_bytes());
        let mut writer = self.writer.borrow_mut();
        assert!(!writer.impl_.is_null());

        // SAFETY:
        // - `writer` must be valid.
        // - `available_bytes()` must be >= `src.len()`.
        unsafe { ptr::copy_nonoverlapping(src.as_ptr(), writer.write_ptr, src.len()) };

        // Advance write pointer.
        writer.write_ptr = writer.write_ptr.wrapping_add(src.len());
    }

    /// Writes bytes from `src` to the writer.
    pub fn append_bytes(&self, src: &[u8]) {
        // Use fast-path if enough bytes are already available.
        if crate::__likely!(src.len() <= self.available_bytes()) {
            self.append_bytes_unchecked(src);
            return;
        }

        let mut writer = self.writer.borrow_mut();
        assert!(!writer.impl_.is_null());

        // SAFETY: `writer` must be valid.
        unsafe {
            PerfettoStreamWriterAppendBytesSlowpath(&mut *writer as *mut _, src.as_ptr(), src.len())
        };
    }

    /// Writes the single byte `value` to the writer.
    pub fn append_byte(&self, value: u8) {
        let mut writer = self.writer.borrow_mut();
        assert!(!writer.impl_.is_null());

        // Create new chunk if needed.
        if crate::__unlikely!(self.available_bytes() < 1) {
            // SAFETY: `writer` must be valid.
            unsafe { PerfettoStreamWriterNewChunk(&mut *writer as *mut _) };
        }

        assert!(1 <= self.available_bytes());

        // SAFETY: `available_bytes()` must be >= 1.
        unsafe { ptr::write(writer.write_ptr, value) };

        // Advance write pointer.
        writer.write_ptr = writer.write_ptr.wrapping_add(1);
    }

    /// Returns a pointer to an area of the chunk long `size` for writing. The
    /// returned area is considered already written by the writer (it will not be
    /// used again).
    ///
    /// SAFETY: `available_bytes()` must be >= `size`.
    #[allow(clippy::mut_from_ref)]
    pub fn reserve_bytes_unchecked(&self, size: usize) -> &mut [u8] {
        assert!(size <= self.available_bytes());
        let mut writer = self.writer.borrow_mut();
        assert!(!writer.impl_.is_null());

        // Get current write pointer.
        let start_ptr = writer.write_ptr;

        // Advance write pointer.
        writer.write_ptr = writer.write_ptr.wrapping_add(size);

        // Create slice for start pointer and size.
        //
        // SAFETY:
        // - `writer` must be valid.
        // - `start_ptr` must be `size` bytes before writer end.
        unsafe { slice::from_raw_parts_mut(start_ptr, size) }
    }

    /// Returns a pointer to an area of the chunk long `size` for writing. The
    /// returned area is considered already written by the writer (it will not be
    /// used again).
    ///
    /// # Safety
    ///
    /// - `size` should be smaller than the chunk size returned by the `delegate`.
    #[allow(clippy::mut_from_ref)]
    pub fn reserve_bytes(&self, size: usize) -> &mut [u8] {
        // Use fast-path if enough bytes are available already.
        if crate::__likely!(size <= self.available_bytes()) {
            return self.reserve_bytes_unchecked(size);
        }

        let mut writer = self.writer.borrow_mut();
        assert!(!writer.impl_.is_null());

        // SAFETY: `writer` must be valid.
        unsafe { PerfettoStreamWriterReserveBytesSlowpath(&mut *writer as *mut _, size) };

        // Get current write pointer.
        let start_ptr = writer.write_ptr.wrapping_sub(size);

        // Create slice for start pointer and size.
        //
        // SAFETY:
        // - `writer` must be valid.
        // - `start_ptr` must be `size` bytes before writer end.
        unsafe { slice::from_raw_parts_mut(start_ptr, size) }
    }

    /// Returns the number of bytes written to the stream writer from the start.
    pub fn get_written_size(&self) -> usize {
        let writer = self.writer.borrow();
        assert!(writer.begin <= writer.write_ptr);

        // SAFETY: `writer.begin` must be <= to `writer.write_ptr`.
        let bytes_written = unsafe { writer.write_ptr.offset_from(writer.begin) as usize };

        writer.written_previously + bytes_written
    }

    /// Returns true if writer is valid.
    pub(crate) fn has_valid_writer(&self) -> bool {
        !self.writer.borrow().impl_.is_null()
    }
}

impl Default for StreamWriter {
    fn default() -> Self {
        Self {
            writer: RefCell::new(PerfettoStreamWriter {
                impl_: ptr::null_mut(),
                begin: ptr::null_mut(),
                end: ptr::null_mut(),
                write_ptr: ptr::null_mut(),
                written_previously: 0,
            }),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn initial_state() {
        let writer = StreamWriter::new();
        assert_eq!(writer.available_bytes(), 0);
        assert_eq!(writer.get_written_size(), 0);
    }
}
