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

use crate::pb_utils::PbWireType;
use perfetto_sdk_sys::*;
use thiserror::Error;

/// Protobuf decoder errors.
#[derive(Error, Debug, PartialEq)]
pub enum PbDecoderError {
    /// Encountered an invalid wire type.
    #[error("Invalid wire type: {0}.")]
    InvalidWireType(u32),
}

/// Protobuf decoder field types.
#[derive(Debug, PartialEq)]
pub enum PbDecoderField<'a> {
    /// Varint field.
    Varint(u64),
    /// Fixed64 field.
    Fixed64(u64),
    /// Delimited field, e.g. nested message or string.
    Delimited(&'a [u8]),
    /// Fixed32 field.
    Fixed32(u32),
}

/// Decoder for parsing protobuf messages.
///
/// Example:
///
/// ```
/// static MSG: &[u8] = b"\x18\x05\x2a\x12\x0a\x05\x68\x65\x6c\x6c\x6f\
///                       \x28\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01";
///
/// for item in perfetto_sdk::pb_decoder::PbDecoder::new(MSG) {
///     // Do something with item
/// }
/// ```
pub struct PbDecoder<'a> {
    decoder: PerfettoPbDecoder,
    _data: &'a [u8],
}

impl<'a> PbDecoder<'a> {
    ///  Create a new decoder instance from data.
    pub fn new(data: &'a [u8]) -> Self {
        let read_ptr = data.as_ptr();
        PbDecoder {
            decoder: PerfettoPbDecoder {
                read_ptr,
                // SAFETY: `data.len()` must be ≤ slice length.
                end_ptr: unsafe { read_ptr.add(data.len()) },
            },
            _data: data,
        }
    }
}

impl<'a> Iterator for PbDecoder<'a> {
    type Item = Result<(u32, PbDecoderField<'a>), PbDecoderError>;

    fn next(&mut self) -> Option<Self::Item> {
        // SAFETY: `self.decoder` must be properly initialized PerfettoPbDecoder struct
        // and done by PbDecoder::new().
        let next: PerfettoPbDecoderField =
            unsafe { PerfettoPbDecoderParseField(&raw mut self.decoder) };
        if next.status != PerfettoPbDecoderStatus_PERFETTO_PB_DECODER_OK {
            return None;
        }

        // SAFETY: `next.wire_type` must match the data stored in `next.value` union,
        // which is expected from a successful call to PerfettoPbDecoderParseField.
        let field = unsafe {
            match PbWireType::try_from(next.wire_type) {
                Ok(PbWireType::Varint) => PbDecoderField::Varint(next.value.integer64),
                Ok(PbWireType::Fixed64) => PbDecoderField::Fixed64(next.value.integer64),
                Ok(PbWireType::Delimited) => {
                    let data = std::slice::from_raw_parts(
                        next.value.delimited.start,
                        next.value.delimited.len,
                    );
                    PbDecoderField::Delimited(data)
                }
                Ok(PbWireType::Fixed32) => PbDecoderField::Fixed32(next.value.integer32),
                Err(_) => {
                    return Some(Err(PbDecoderError::InvalidWireType(next.wire_type)));
                }
            }
        };

        Some(Ok((next.id, field)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty() {
        let decoder = PbDecoder::new(&[]);
        let items: Vec<_> = decoder.collect();
        assert_eq!(items.len(), 0);
    }

    // # proto-message: perfetto.protos.TestEvent
    // counter: 5
    // payload {
    //   str: "hello"
    //   single_int: -1
    // }
    static MSG: &[u8] = b"\x18\x05\x2a\x12\x0a\x05\x68\x65\x6c\x6c\x6f\
                          \x28\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01";

    #[test]
    fn test_event() {
        use PbDecoderField::*;
        let decoder = PbDecoder::new(MSG);
        let items: Vec<_> = decoder.collect();
        assert_eq!(items[0], Ok((3, Varint(5))));
        match &items[1] {
            Ok((5, Delimited(data))) => {
                let payload_decoder = PbDecoder::new(data);
                let payload_items: Vec<_> = payload_decoder.collect();
                assert_eq!(payload_items[0], Ok((1, Delimited(b"hello"))));
                assert_eq!(payload_items[1], Ok((5, Varint(-1i64 as u64))));
            }
            other => panic!("unexpected item: {:?}", other),
        }
    }
}
