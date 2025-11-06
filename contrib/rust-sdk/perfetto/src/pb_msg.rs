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

use crate::pb_utils::*;
use crate::stream_writer::StreamWriter;
use perfetto_sdk_sys::*;
use std::{
    cell::RefCell,
    ptr,
    rc::{Rc, Weak},
};
use thiserror::Error;

/// Protobuf message errors.
#[derive(Error, Debug, PartialEq)]
pub enum PbMsgError {
    /// No output for writer.
    #[error("Message writer is missing an output.")]
    MissingOutputForWriter,
}

/// Reference to the memory used by a `PbMsg` for writing.
#[derive(Default)]
pub struct PbMsgWriter {
    pub(crate) writer: StreamWriter,
}

impl PbMsgWriter {
    /// Creates a new protobuf message writer.
    pub fn new() -> Self {
        Self::default()
    }
}

// The number of bytes reserved by this implementation to encode a protobuf type
// 2 field size as var-int. Keep this in sync with kMessageLengthFieldSize in
// proto_utils.h.
const PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE: usize = 4;

struct PbMsgSizeField {
    ptr: *mut u8,
    parent: Weak<RefCell<PbMsgSizeField>>,
}

impl PbMsgSizeField {
    pub fn patch(&mut self, writer: &PbMsgWriter) {
        assert!(!self.ptr.is_null());
        let mut writer = writer.writer.writer.borrow_mut();
        // SAFETY:
        // - `writer` must be a properly initialized PerfettoStreamWriter struct.
        // - `self.ptr` must be pointing to a `PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE` sized buffer.
        self.ptr = unsafe { PerfettoStreamWriterAnnotatePatch(&mut *writer as *mut _, self.ptr) };
    }

    pub fn patch_stack(&mut self, writer: &PbMsgWriter) {
        let (range_begin, range_end) = {
            let inner_writer = writer.writer.writer.borrow();
            (
                inner_writer.begin as *const u8,
                inner_writer.end as *const u8,
            )
        };
        if range_begin <= self.ptr && (self.ptr as *const u8) < range_end {
            self.patch(writer);
            if let Some(parent) = self.parent.upgrade() {
                parent.borrow_mut().patch_stack(writer);
            }
        }
    }
}

/// Protobuf message struct.
pub struct PbMsg<'a> {
    size_field: Rc<RefCell<PbMsgSizeField>>,
    size: usize,
    writer: &'a PbMsgWriter,
}

impl<'a> PbMsg<'a> {
    /// Creates a new message struct using `writer`.
    pub fn new(writer: &'a PbMsgWriter) -> Result<Self, PbMsgError> {
        if !writer.writer.has_valid_writer() {
            return Err(PbMsgError::MissingOutputForWriter);
        }
        Ok(Self {
            size_field: Rc::new(RefCell::new(PbMsgSizeField {
                ptr: std::ptr::null_mut(),
                parent: Weak::<RefCell<PbMsgSizeField>>::default(),
            })),
            size: 0,
            writer,
        })
    }

    /// Append bytes to message.
    pub fn append_bytes(&mut self, bytes: &[u8]) {
        if crate::__unlikely!(bytes.len() > self.writer.writer.available_bytes()) {
            self.size_field.borrow_mut().patch_stack(self.writer);
        }
        self.writer.writer.append_bytes(bytes);
        self.size += bytes.len();
    }

    /// Append byte to message.
    pub fn append_byte(&mut self, value: u8) {
        self.append_bytes(&[value]);
    }

    /// Append varint to message.
    pub fn append_varint(&mut self, value: u64) {
        let mut buf: [u8; PB_VARINT_MAX_SIZE_64] = [0; PB_VARINT_MAX_SIZE_64];
        let written = pb_write_varint(value, &mut buf);
        self.append_bytes(&buf[..written]);
    }

    /// Append fixed32 to message.
    pub fn append_fixed32(&mut self, value: u32) {
        let mut buf: [u8; 4] = [0; 4];
        pb_write_fixed32(value, &mut buf);
        self.append_bytes(&buf);
    }

    /// Append fixed64 to message.
    pub fn append_fixed64(&mut self, value: u64) {
        let mut buf: [u8; 8] = [0; 8];
        pb_write_fixed64(value, &mut buf);
        self.append_bytes(&buf);
    }

    /// Append varint field to message.
    pub fn append_type0_field(&mut self, field_id: u32, value: u64) {
        const BUF_SIZE: usize = PB_VARINT_MAX_SIZE_32 + PB_VARINT_MAX_SIZE_64;
        let mut buf: [u8; BUF_SIZE] = [0; BUF_SIZE];
        let tag = pb_make_tag(field_id, PbWireType::Varint);
        let mut written = pb_write_varint(tag.into(), &mut buf);
        written += pb_write_varint(value, &mut buf[written..]);
        self.append_bytes(&buf[..written]);
    }

    /// Append delimited field to message.
    pub fn append_type2_field(&mut self, field_id: u32, data: &[u8]) {
        const BUF_SIZE: usize = PB_VARINT_MAX_SIZE_32 + PB_VARINT_MAX_SIZE_64;
        let mut buf: [u8; BUF_SIZE] = [0; BUF_SIZE];
        let tag = pb_make_tag(field_id, PbWireType::Delimited);
        let mut written = pb_write_varint(tag.into(), &mut buf);
        written += pb_write_varint(data.len() as u64, &mut buf[written..]);
        self.append_bytes(&buf[..written]);
        self.append_bytes(data);
    }

    /// Append fixed32 field to message.
    pub fn append_fixed32_field(&mut self, field_id: u32, value: u32) {
        const BUF_SIZE: usize = PB_VARINT_MAX_SIZE_32 + 4;
        let mut buf: [u8; BUF_SIZE] = [0; BUF_SIZE];
        let tag = pb_make_tag(field_id, PbWireType::Fixed32);
        let mut written = pb_write_varint(tag.into(), &mut buf);
        written += pb_write_fixed32(value, &mut buf[written..]);
        self.append_bytes(&buf[..written]);
    }

    /// Append float field to message.
    pub fn append_float_field(&mut self, field_id: u32, value: f32) {
        self.append_fixed32_field(field_id, pb_float_to_fixed32(value));
    }

    /// Append fixed64 field to message.
    pub fn append_fixed64_field(&mut self, field_id: u32, value: u64) {
        const BUF_SIZE: usize = PB_VARINT_MAX_SIZE_32 + 8;
        let mut buf: [u8; BUF_SIZE] = [0; BUF_SIZE];
        let tag = pb_make_tag(field_id, PbWireType::Fixed64);
        let mut written = pb_write_varint(tag.into(), &mut buf);
        written += pb_write_fixed64(value, &mut buf[written..]);
        self.append_bytes(&buf[..written]);
    }

    /// Append doubles field to message.
    pub fn append_double_field(&mut self, field_id: u32, value: f64) {
        self.append_fixed64_field(field_id, pb_double_to_fixed64(value));
    }

    /// Append C string field to message.
    pub fn append_cstr_field(&mut self, field_id: u32, c_str: &str) {
        self.append_type2_field(field_id, c_str.as_bytes());
    }

    /// Append nested message to message.
    pub fn append_nested<F>(&mut self, field_id: u32, mut cb: F)
    where
        F: FnMut(&mut PbMsg),
    {
        let tag = pb_make_tag(field_id, PbWireType::Delimited);
        self.append_varint(tag.into());
        if crate::__unlikely!(
            PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE > self.writer.writer.available_bytes()
        ) {
            self.size_field.borrow_mut().patch_stack(self.writer);
        }
        let size_field_bytes = self
            .writer
            .writer
            .reserve_bytes(PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE);
        self.size += PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE;
        let mut nested = PbMsg {
            size_field: Rc::new(RefCell::new(PbMsgSizeField {
                ptr: size_field_bytes.as_mut_ptr(),
                parent: Rc::downgrade(&self.size_field),
            })),
            size: 0,
            writer: self.writer,
        };
        cb(&mut nested);
        self.size += nested.finalize();
    }

    /// Finalize message and return size.
    pub fn finalize(&mut self) -> usize {
        // Write the length of the nested message a posteriori, using a leading-zero
        // redundant varint encoding.
        if !self.size_field.borrow().ptr.is_null() {
            let mut size_to_write = self.size;
            for i in 0..PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE {
                let msb: u8 = if i < 3 { 0x80 } else { 0 };
                // SAFETY: `self.size_field` must point to a
                // `PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE` sized buffer.
                unsafe {
                    self.size_field
                        .borrow_mut()
                        .ptr
                        .add(i)
                        .write((size_to_write & 0xff) as u8 | msb)
                };
                size_to_write >>= 7;
            }
            self.size_field.borrow_mut().ptr = ptr::null_mut();
        }
        self.size
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::heap_buffer::HeapBuffer;
    use std::error::Error;

    #[test]
    fn append_bytes() -> Result<(), Box<dyn Error>> {
        let writer = PbMsgWriter::new();
        let hb = HeapBuffer::new(&writer.writer);
        let mut msg = PbMsg::new(&writer)?;
        msg.append_bytes(b"ok");
        let size = msg.finalize();
        assert_eq!(size, 2);
        let written_size = writer.writer.get_written_size();
        assert_eq!(written_size, 2);
        let mut result: Vec<u8> = vec![0u8; written_size];
        hb.copy_into(&mut result);
        assert_eq!(result, [111, 107]);
        Ok(())
    }

    #[test]
    fn append_nested() -> Result<(), Box<dyn Error>> {
        let writer = PbMsgWriter::new();
        let hb = HeapBuffer::new(&writer.writer);
        let mut msg = PbMsg::new(&writer)?;
        msg.append_bytes(b"foo");
        msg.append_nested(3, |msg| {
            msg.append_cstr_field(10, "bar");
        });
        let size = msg.finalize();
        assert_eq!(size, 13);
        let written_size = writer.writer.get_written_size();
        assert_eq!(written_size, 13);
        let mut result: Vec<u8> = vec![0u8; written_size];
        hb.copy_into(&mut result);
        assert_eq!(result, [
            102, 111, 111, 26, 133, 128, 128, 0, 82, 3, 98, 97, 114
        ]);
        Ok(())
    }
}
