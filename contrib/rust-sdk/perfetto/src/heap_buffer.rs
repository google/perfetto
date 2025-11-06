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

use crate::stream_writer::StreamWriter;
use perfetto_sdk_sys::*;
use std::os::raw::c_void;

/// A HeapBuffer can be used to serialize protobuf data using the
/// StreamWriter interface. Stores data on heap allocated buffers, which
/// can be read back with copy_into().
pub struct HeapBuffer<'a> {
    buffer: *mut PerfettoHeapBuffer,
    writer: &'a StreamWriter,
}

impl<'a> HeapBuffer<'a> {
    /// Creates a HeapBuffer. Takes a reference to a StreamWriter.
    /// The StreamWriter can be used later to serialize protobuf data.
    pub fn new(writer: &'a StreamWriter) -> Self {
        let mut inner_writer = writer.writer.borrow_mut();
        // SAFETY:
        // - `inner_writer` must be a pointer to a properly initialized
        //   PerfettoStreamWriter struct, which the StreamWriter interface
        //   is guaranteed to provide.
        let buffer = unsafe { PerfettoHeapBufferCreate(&mut *inner_writer as *mut _) };
        HeapBuffer { buffer, writer }
    }

    /// Copies data from the heap buffer to `dst`.
    pub fn copy_into(&self, dst: &mut [u8]) {
        let mut inner_writer = self.writer.writer.borrow_mut();
        // SAFETY:
        // - `self.buffer` must have been created with PerfettoHeapBufferCreate.
        // - `inner_writer` must be a pointer to a properly initialized
        //   PerfettoStreamWriter struct, which the StreamWriter interface
        //   is guaranteed to provide.
        unsafe {
            PerfettoHeapBufferCopyInto(
                self.buffer,
                &mut *inner_writer as *mut _,
                dst.as_mut_ptr() as *mut c_void,
                dst.len(),
            )
        };
    }
}

impl Drop for HeapBuffer<'_> {
    fn drop(&mut self) {
        let mut inner_writer = self.writer.writer.borrow_mut();
        // SAFETY:
        // - `self.buffer` must have been created with PerfettoHeapBufferCreate.
        // - `inner_writer` must be a pointer to a properly initialized
        //   PerfettoStreamWriter struct, which the StreamWriter interface
        //   is guaranteed to provide.
        unsafe { PerfettoHeapBufferDestroy(self.buffer, &mut *inner_writer as *mut _) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn init() {
        let writer = StreamWriter::new();
        let _hb = HeapBuffer::new(&writer);
    }
}
