---
type: regex
pattern: java\.lang\.String|ICUResourceBundle|ICU|icu\.
---
Ground truth (skill triage_dominator_path.sql and class summary tree): the
largest dominated path is [ROOT_INTERNED_STRING] -> java.lang.String,
52,300 objects, 2.39 MB self size. Next is the ICUResourceBundleReader
ReaderCache chain (~2.1 MB retained). The whole reachable heap is a few
MB; there is no strong leak signal. By retained (dominator) size the top
retainer is Android's ICU data (~6.4 MB grouped, ReaderCache chain ~2.1 MB),
so naming either interned Strings or ICU is correct.
