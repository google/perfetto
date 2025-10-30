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

use {
    quote::quote,
    syn::{
        AttributeArgs, Error, ItemFn, Lit, Meta, NestedMeta, parse_macro_input, spanned::Spanned,
    },
};

#[derive(Debug, Default)]
struct MacroArgs {
    category: Option<String>,
    prefix: Option<String>,
    flush: bool,
}

impl MacroArgs {
    fn from_list(args: &[NestedMeta]) -> Result<Self, Error> {
        let mut category: Option<String> = None;
        let mut prefix: Option<String> = None;
        let mut flush = false;
        for arg in args {
            match arg {
                NestedMeta::Lit(Lit::Str(lit)) => {
                    if category.is_some() {
                        return Err(Error::new(lit.span(), "duplicate `category` argument"));
                    }
                    category = Some(lit.value());
                }
                NestedMeta::Meta(Meta::NameValue(meta)) if meta.path.is_ident("prefix") => {
                    if let Lit::Str(litstr) = &meta.lit {
                        prefix = Some(litstr.value());
                    } else {
                        return Err(Error::new(
                            meta.lit.span(),
                            "expected a string literal for `prefix`",
                        ));
                    }
                }
                NestedMeta::Meta(Meta::NameValue(meta)) if meta.path.is_ident("flush") => {
                    if let Lit::Bool(litbool) = &meta.lit {
                        flush = litbool.value();
                    } else {
                        return Err(Error::new(
                            meta.lit.span(),
                            "expected a boolean literal for `flush`",
                        ));
                    }
                }
                _ => {
                    return Err(Error::new(arg.span(), "unknown attribute argument"));
                }
            }
        }
        Ok(Self {
            category,
            prefix,
            flush,
        })
    }
}

/// This provides a helper proc macro to trace function calls.
///
/// Example:
///
///  ```
/// use perfetto::*;
///
/// track_event_categories! {
///     pub mod my_derive_te_ns {
///         ( "c1", "Category 1", [] ),
///     }
/// }
///
/// use my_derive_te_ns as perfetto_te_ns;
///
/// use perfetto_derive::tracefn;
///
/// #[tracefn("c1")]
/// fn atoi(string_arg: String) -> Result<i32, std::num::ParseIntError> {
///     string_arg.parse::<i32>()
/// }
///
/// use std::error::Error;
///
/// fn main() -> Result<(), Box<dyn Error>> {
///     producer::Producer::init(
///         producer::ProducerInitArgsBuilder::new()
///             .backends(producer::Backends::SYSTEM)
///             .build(),
///     );
///     track_event::TrackEvent::init();
///     perfetto_te_ns::register()?;
///     let result = atoi(1234.to_string())?;
///     assert_eq!(result, 1234);
///     Ok(())
/// }
/// ```
#[proc_macro_attribute]
pub fn tracefn(
    attr: proc_macro::TokenStream,
    item: proc_macro::TokenStream,
) -> proc_macro::TokenStream {
    let input = parse_macro_input!(item as ItemFn);
    let attr_args = parse_macro_input!(attr as AttributeArgs);
    let macro_args = match MacroArgs::from_list(&attr_args) {
        Ok(v) => v,
        Err(e) => return e.to_compile_error().into(),
    };
    let fn_name = &input.sig.ident;
    let fn_args = &input.sig.inputs;
    let fn_output = &input.sig.output;
    let fn_body = &input.block;
    let fn_abi = &input.sig.abi;
    let fn_vis = &input.vis;
    let fn_attrs = &input.attrs;
    let Some(category) = macro_args.category else {
        return Error::new_spanned(&input.sig.ident, "missing required `category` argument")
            .to_compile_error()
            .into();
    };
    let name = if let Some(prefix) = macro_args.prefix {
        prefix + &fn_name.to_string()
    } else {
        fn_name.to_string()
    };
    let flush = macro_args.flush;
    let args = fn_args.iter().map(|arg| match arg {
        syn::FnArg::Typed(pat_type) => {
            let arg_name = &pat_type.pat;
            quote! {
                (stringify!(#arg_name).to_string(), format!("{:?}", #arg_name))
            }
        }
        _ => panic!("unhandled arg type"),
    });
    let result = quote! {
        #( #fn_attrs )*
        #fn_vis #fn_abi fn #fn_name(#fn_args) #fn_output {
            use perfetto::track_event::*;
            use std::os::raw::c_char;
            const CATEGORY_INDEX: usize = perfetto_te_ns::category_index(#category);
            let is_category_enabled = perfetto_te_ns::is_category_enabled(CATEGORY_INDEX);
            if is_category_enabled {
                let mut ctx = EventContext::default();
                let args = [#(#args),*];
                for arg in &args {
                    ctx.add_debug_arg(&arg.0, TrackEventDebugArg::String(&arg.1));
                }
                perfetto_te_ns::emit(
                    CATEGORY_INDEX,
                    TrackEventType::SliceBegin(concat!(#name, "\0").as_ptr() as *const c_char),
                    &mut ctx,
                );
            }
            let result = (|| #fn_body)();
            if is_category_enabled {
                let mut ctx = EventContext::default();
                if #flush {
                    ctx.set_flush();
                }
                perfetto_te_ns::emit(CATEGORY_INDEX, TrackEventType::SliceEnd, &mut ctx);
            }
            result
        }
    };
    result.into()
}
