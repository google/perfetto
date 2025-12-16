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
    syn::{Error, Expr, ExprLit, ItemFn, Lit, Token, parse_macro_input, punctuated::Punctuated},
};

#[derive(Debug, Default)]
struct MacroArgs {
    category: Option<String>,
    prefix: Option<String>,
    flush: bool,
}

impl MacroArgs {
    fn from_exprs(exprs: &Punctuated<Expr, Token![,]>) -> Result<Self, Error> {
        let mut category: Option<String> = None;
        let mut prefix: Option<String> = None;
        let mut flush = false;
        for expr in exprs {
            match expr {
                Expr::Lit(ExprLit {
                    lit: Lit::Str(s), ..
                }) => {
                    if category.is_some() {
                        return Err(Error::new_spanned(s, "duplicate `category` argument"));
                    }
                    category = Some(s.value());
                }
                Expr::Assign(assign) => {
                    if let Expr::Path(path) = &*assign.left {
                        if path.path.is_ident("prefix") {
                            if let Expr::Lit(ExprLit {
                                lit: Lit::Str(s), ..
                            }) = &*assign.right
                            {
                                prefix = Some(s.value());
                            } else {
                                return Err(Error::new_spanned(
                                    &*assign.right,
                                    "expected string literal, e.g., prefix = \"toplevel\"",
                                ));
                            }
                        } else if path.path.is_ident("flush") {
                            if let Expr::Lit(ExprLit {
                                lit: Lit::Bool(b), ..
                            }) = &*assign.right
                            {
                                flush = b.value();
                            } else {
                                return Err(Error::new_spanned(
                                    &*assign.right,
                                    "expected boolean literal, e.g., flush = true",
                                ));
                            }
                        } else {
                            return Err(Error::new_spanned(path, "invalid attribute argument"));
                        }
                    } else {
                        return Err(Error::new_spanned(
                            &*assign.left,
                            "invalid left-hand side; expected identifier",
                        ));
                    }
                }
                _ => {
                    return Err(Error::new_spanned(expr, "unknown attribute expression"));
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
/// use perfetto_sdk::*;
///
/// track_event_categories! {
///     pub mod my_derive_te_ns {
///         ( "c1", "Category 1", [] ),
///     }
/// }
///
/// use my_derive_te_ns as perfetto_te_ns;
///
/// use perfetto_sdk_derive::tracefn;
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
    let attr_exprs = parse_macro_input!(attr with Punctuated<Expr, Token![,]>::parse_terminated);
    let macro_args = match MacroArgs::from_exprs(&attr_exprs) {
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
            use perfetto_sdk::track_event::*;
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
