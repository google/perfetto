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

/// `common` protobufs.
pub mod common;

/// `config` protobufs.
pub mod config;

/// `trace` protobufs.
#[allow(clippy::module_inception)]
pub mod trace;

/// Defines a protobuf enum.
#[macro_export]
macro_rules! pb_enum {
    (
        $name:ident {
            $( $entry:ident : $id:literal ),+ $(,)?
        }
    ) => {
        paste::paste! {
            #[doc = concat!("Protobuf enum for `", stringify!($name), "`")]
            #[allow(non_camel_case_types)]
            #[repr(u32)]
            #[derive(Debug, Clone, Copy, PartialEq, Eq)]
            pub enum $name {
                $(
                    #[doc = concat!("Variant for `", stringify!($entry), "`")]
                    [<$entry:camel>] = $id
                ),*
            }

            impl From<$name> for u32 {
                #[inline]
                fn from(v: $name) -> u32 { v as u32 }
            }

            impl TryFrom<u32> for $name {
                type Error = ();
                fn try_from(v: u32) -> Result<Self, Self::Error> {
                    match v {
                        $(
                            $id => Ok([<$name>]::[<$entry:camel>]),
                        )*
                        _ => Err(()),
                    }
                }
            }
        }
    };
}

/// Defines a protobuf message.
///
/// Defines the type for a protobuf message. `name` is the name of the message type.
#[macro_export]
macro_rules! pb_msg {
    (
        $name:ident {
            $( $field:ident : $tp:tt, $kind:ident, $id:literal ),+ $(,)?
        }
    ) => {
        paste::paste! {
            #[doc = concat!("Protobuf field numbers for `", stringify!($name), "`")]
            #[repr(u32)]
            pub enum [<$name:camel FieldNumber>] {
                $(
                    #[doc = concat!("Field number for `", stringify!($field), "`")]
                    [<$field:camel>] = $id
                ),*
            }
        }

        paste::paste! {
            #[doc = concat!("Protobuf message struct for `", stringify!($name), "`")]
            #[allow(non_camel_case_types)]
            pub struct $name<'a, 'b> {
                #[doc = concat!("PbMsg for protobuf message `", stringify!($name), "`")]
                pub msg: &'a mut $crate::pb_msg::PbMsg<'b>,
            }
        }

        impl<'a, 'b> $name<'a, 'b> {
            $(
                pb_msg!(@setter $name, $field, $id, $kind, $tp);
            )*
        }
    };

    // Cstr
    (@setter $name:ident, $field:ident, $id: literal, primitive, String) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            pub fn [<set_ $field>] (&mut self, value: impl Into<String>) -> &mut Self {
                let s: String = value.into();
                self.msg.append_type2_field($id, s.as_bytes());
                self
            }
        }
    };

    // float
    (@setter $name:ident, $field:ident, $id: literal, primitive, f32) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            pub fn [<set_ $field>] (&mut self, value: f32) -> &mut Self {
                self.msg.append_float_field($id, value);
                self
            }
        }
    };

    // double
    (@setter $name:ident, $field:ident, $id: literal, primitive, f64) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            pub fn [<set_ $field>] (&mut self, value: f64) -> &mut Self {
                self.msg.append_double_field($id, value);
                self
            }
        }
    };

    // Varint
    (@setter $name:ident, $field:ident, $id: literal, primitive, u32) => {
        pb_msg!(@varint_setter $name, $field, $id, u32);
    };
    (@setter $name:ident, $field:ident, $id: literal, primitive, u64) => {
        pb_msg!(@varint_setter $name, $field, $id, u64);
    };
    (@setter $name:ident, $field:ident, $id: literal, primitive, i32) => {
        pb_msg!(@varint_setter $name, $field, $id, i32);
    };
    (@setter $name:ident, $field:ident, $id: literal, primitive, i64) => {
        pb_msg!(@varint_setter $name, $field, $id, i64);
    };
    (@setter $name:ident, $field:ident, $id: literal, primitive, bool) => {
        pb_msg!(@varint_setter $name, $field, $id, bool);
    };

    (@varint_setter $name:ident, $field:ident, $id: literal, $tp:tt) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            pub fn [<set_ $field>] (&mut self, value: $tp) -> &mut Self {
                self.msg.append_type0_field($id, value as u64);
                self
            }
        }
    };

    // Enum
    (@setter $name:ident, $field:ident, $id: literal, enum, $tp:tt) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            pub fn [<set_ $field>] (&mut self, value: $tp) -> &mut Self {
                self.msg.append_type0_field($id, value as u64);
                self
            }
        }
    };

    // Fallback to message
    (@setter $name:ident, $field:ident, $id: literal, msg, $tp:tt) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            pub fn [<set_ $field>] <F>(&mut self, cb: F) -> &mut Self
            where
                F: for<'p> Fn(&'p mut $tp),
            {
                self.msg.append_nested($id, |nested_msg| {
                    let mut msg_field: $tp<'_, '_> = $tp {
                        msg: nested_msg,
                    };
                    cb(&mut msg_field);
                });
                self
            }
        }
    };
}
