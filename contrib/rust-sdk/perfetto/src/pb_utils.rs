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

use std::convert::TryFrom;

/// Type of fields that can be found in a protobuf serialized message.
#[repr(u32)]
pub enum PbWireType {
    /// Variable-length integer.
    Varint = 0,
    /// Fixed 8-byte value.
    Fixed64 = 1,
    /// Length-delimited. Prefixed by a varint length, followed by that many bytes.
    Delimited = 2,
    /// Fixed 4-byte value.
    Fixed32 = 5,
}

impl TryFrom<u32> for PbWireType {
    type Error = ();

    fn try_from(value: u32) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(PbWireType::Varint),
            1 => Ok(PbWireType::Fixed64),
            2 => Ok(PbWireType::Delimited),
            5 => Ok(PbWireType::Fixed32),
            _ => Err(()),
        }
    }
}

/// Creates a field tag, which encodes the field type and the field id.
pub fn pb_make_tag(field_id: u32, wire_type: PbWireType) -> u32 {
    (field_id << 3) | wire_type as u32
}

/// Maximum bytes size of a 64-bit integer encoded as a VarInt.
pub const PB_VARINT_MAX_SIZE_64: usize = 10;

/// Maximum bytes size of a 32-bit integer encoded as a VarInt.
pub const PB_VARINT_MAX_SIZE_32: usize = 5;

/// Encodes `value` as a VarInt into `*dst`.
///
/// `dst` must be large enough to represent `value`:
/// PERFETTO_PB_VARINT_MAX_SIZE_* can help.
pub fn pb_write_varint(value: u64, dst: &mut [u8]) -> usize {
    let mut cur_value = value;
    let mut offset: usize = 0;
    let mut byte: u8;
    while cur_value >= 0x80 {
        byte = ((cur_value & 0x7f) | 0x80) as u8;
        dst[offset] = byte;
        offset += 1;
        cur_value >>= 7;
    }
    byte = (cur_value & 0x7f) as u8;
    dst[offset] = byte;
    offset += 1;
    offset
}

/// Encodes `value` as a fixed32 (little endian) into `*dst`.
///
/// `dst` must have at least 4 bytes of space.
pub fn pb_write_fixed32(value: u32, dst: &mut [u8]) -> usize {
    dst[0] = value as u8;
    dst[1] = (value >> 8) as u8;
    dst[2] = (value >> 16) as u8;
    dst[3] = (value >> 24) as u8;
    4
}

/// Encodes `value` as a fixed64 (little endian) into `*dst`.
///
/// `dst` must have at least 8 bytes of space.
pub fn pb_write_fixed64(value: u64, dst: &mut [u8]) -> usize {
    dst[0] = value as u8;
    dst[1] = (value >> 8) as u8;
    dst[2] = (value >> 16) as u8;
    dst[3] = (value >> 24) as u8;
    dst[4] = (value >> 32) as u8;
    dst[5] = (value >> 40) as u8;
    dst[6] = (value >> 48) as u8;
    dst[7] = (value >> 56) as u8;
    8
}

/// Parses a VarInt from the encoded buffer |src|.
/// The parsed int value is returned in the output arg |value|. Returns the
/// parsed int and the number of consumed bytes, or a pair of zeros if the
/// VarInt could not be fully parsed because there was not enough space in the
/// buffer.
pub fn pb_parse_varint(src: &[u8]) -> (u64, usize) {
    let mut offset: usize = 0;
    let mut value: u64 = 0;
    let mut shift: u32 = 0;
    while offset < src.len() && shift < 64 {
        let byte = src[offset];
        offset += 1;
        value |= ((byte & 0x7f) as u64) << shift;
        if (byte & 0x80) == 0 {
            // In valid cases we get here.
            return (value, offset);
        }
        shift += 7;
    }
    (0, 0)
}

/// ZigZag encodes 4-byte `value`.
pub fn pb_zigzag_encode32(value: i32) -> u32 {
    ((value as u32) << 1) ^ (value >> 31) as u32
}

/// ZigZag encodes 8-byte `value`.
pub fn pb_zigzag_encode64(value: i64) -> u64 {
    ((value as u64) << 1) ^ (value >> 63) as u64
}

/// ZigZag decodes 4-byte `value`.
pub fn pb_zigzag_decode32(value: u32) -> i32 {
    let mask: u32 = (-((value & 1) as i32)) as u32;
    ((value >> 1) ^ mask) as i32
}

/// ZigZag decodes 8-byte `value`.
pub fn pb_zigzag_decode64(value: u64) -> i64 {
    let mask: u64 = (-((value & 1) as i64)) as u64;
    ((value >> 1) ^ mask) as i64
}

/// Converts `value` to fixed32.
pub fn pb_float_to_fixed32(value: f32) -> u32 {
    u32::from_ne_bytes(value.to_ne_bytes())
}

/// Converts `value` to fixed64.
pub fn pb_double_to_fixed64(value: f64) -> u64 {
    u64::from_ne_bytes(value.to_ne_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn make_tag() {
        assert_eq!(pb_make_tag(4, PbWireType::Fixed32), 37);
    }

    #[test]
    fn write_varint() {
        let mut buf: [u8; PB_VARINT_MAX_SIZE_64] = [0; PB_VARINT_MAX_SIZE_64];
        assert_eq!(pb_write_varint(1234, &mut buf), 2);
        assert_eq!(buf, [210, 9, 0, 0, 0, 0, 0, 0, 0, 0]);
        let (value, size) = pb_parse_varint(&buf);
        assert_eq!(value, 1234);
        assert_eq!(size, 2);
    }

    #[test]
    fn write_fixed32() {
        let mut buf: [u8; 4] = [0; 4];
        assert_eq!(pb_write_fixed32(0xfffffff, &mut buf), 4);
        assert_eq!(buf, [255, 255, 255, 15]);
    }

    #[test]
    fn write_fixed64() {
        let mut buf: [u8; 8] = [0; 8];
        assert_eq!(pb_write_fixed64(0xffffffffffffff, &mut buf), 8);
        assert_eq!(buf, [255, 255, 255, 255, 255, 255, 255, 0]);
    }

    #[test]
    fn zigzag32() {
        assert_eq!(pb_zigzag_encode32(-132323), 264645);
        assert_eq!(pb_zigzag_decode32(264645), -132323);
    }

    #[test]
    fn zigzag64() {
        assert_eq!(pb_zigzag_encode64(82783), 165566);
        assert_eq!(pb_zigzag_decode64(165566), 82783);
    }
}
