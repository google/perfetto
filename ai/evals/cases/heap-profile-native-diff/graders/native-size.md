---
type: regex
pattern: "13[.,][58]\\d*\\s*MB|14,?4[0-9]{4}|14,?1[0-9]{4}"
---
Ground truth: native `libc.malloc` allocations increased from ~294.9 KB (18
allocations) in the before trace to ~13.82 MB (425 allocations) in the after
trace (+13.52 MB allocated churn, +13.50 MB unreleased heap).
