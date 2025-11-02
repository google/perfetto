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
                pb_msg!(@setter pub fn $name, $field, $id, $kind, $tp);
            )*
        }
    };

    // Cstr
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, String) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: impl Into<String>) -> &mut Self;
        }
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, String) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: impl Into<String>) -> &mut Self {
                let s: String = value.into();
                self.msg.append_type2_field($id, s.as_bytes());
                self
            }
        }
    };

    // float
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, String) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: f32) -> &mut Self;
        }
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, f32) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: f32) -> &mut Self {
                self.msg.append_float_field($id, value);
                self
            }
        }
    };

    // double
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, String) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: f64) -> &mut Self;
        }
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, f64) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: f64) -> &mut Self {
                self.msg.append_double_field($id, value);
                self
            }
        }
    };

    // Varint
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, u32) => {
        pb_msg!(@varint_decl $vis fn $name, $field, $id, u32);
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, u32) => {
        pb_msg!(@varint_setter $vis fn $name, $field, $id, u32);
    };
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, u64) => {
        pb_msg!(@varint_decl $vis fn $name, $field, $id, u64);
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, u64) => {
        pb_msg!(@varint_setter $vis fn $name, $field, $id, u64);
    };
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, i32) => {
        pb_msg!(@varint_decl $vis fn $name, $field, $id, i32);
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, i32) => {
        pb_msg!(@varint_setter $vis fn $name, $field, $id, i32);
    };
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, i64) => {
        pb_msg!(@varint_decl $vis fn $name, $field, $id, i64);
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, i64) => {
        pb_msg!(@varint_setter $vis fn $name, $field, $id, i64);
    };
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, bool) => {
        pb_msg!(@varint_decl $vis fn $name, $field, $id, bool);
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, primitive, bool) => {
        pb_msg!(@varint_setter $vis fn $name, $field, $id, bool);
    };

    (@varint_decl $vis:vis fn $name:ident, $field:ident, $id: literal, $tp:tt) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: $tp) -> &mut Self;
        }
    };
    (@varint_setter $vis:vis fn $name:ident, $field:ident, $id: literal, $tp:tt) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: $tp) -> &mut Self {
                self.msg.append_type0_field($id, value as u64);
                self
            }
        }
    };

    // Enum
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, enum, $tp:tt) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: $tp) -> &mut Self;
        }
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, enum, $tp:tt) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] (&mut self, value: $tp) -> &mut Self {
                self.msg.append_type0_field($id, value as u64);
                self
            }
        }
    };

    // Fallback to message
    (@decl $vis:vis fn $name:ident, $field:ident, $id: literal, msg, $tp:tt) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] <F>(&mut self, cb: F) -> &mut Self
            where
                F: for<'p> Fn(&'p mut $tp);
        }
    };
    (@setter $vis:vis fn $name:ident, $field:ident, $id: literal, msg, $tp:tt) => {
        paste::paste! {
            #[doc = concat!("Set `", stringify!($field), "` field")]
            $vis fn [<set_ $field>] <F>(&mut self, cb: F) -> &mut Self
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

/// Defines extra fields for a protobuf message.
#[macro_export]
macro_rules! pb_msg_ext {
    (
        $name:ident {
            $( $field:ident : $tp:tt, $kind:ident, $id:literal ),+ $(,)?
        }
    ) => {
        paste::paste! {
            #[doc = concat!("Protobuf extra field numbers for `", stringify!($name), "`")]
            #[repr(u32)]
            pub enum [<$name:camel ExtFieldNumber>] {
                $(
                    #[doc = concat!("Field number for `", stringify!($field), "`")]
                    [<$field:camel>] = $id
                ),*
            }
        }

        paste::paste! {
            #[doc = concat!("Protobuf extra message trait for `", stringify!($name), "`")]
            #[allow(non_camel_case_types)]
            pub trait [<$name Ext>]<'a, 'b> {
                $(
                    pb_msg!(@decl fn $name, $field, $id, $kind, $tp);
                )*
            }

            impl<'a, 'b> [<$name Ext>]<'_, '_> for $name<'a, 'b> {
                $(
                    pb_msg!(@setter fn $name, $field, $id, $kind, $tp);
                )*
            }
        }
    };
}
