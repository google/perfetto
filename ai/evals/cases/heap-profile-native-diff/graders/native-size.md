---
type: regex
pattern: "13,520,896|13,815,808|13\\.52 MB"
---
Ground truth: native `libc.malloc` total allocation churn (`SUM(MAX(size, 0))`,
excluding negative free records) increased by +13,520,896 B (+13.52 MB), from
294,912 B (18 allocations) to 13,815,808 B (425 allocations). Summing raw `size`
gives unreleased bytes (+13,504,512 B), not allocation churn.
