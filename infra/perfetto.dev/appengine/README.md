# www.perfetto.dev Cloud scripts

The docs site follows the same architecture of ui.perfetto.dev.
See [go/perfetto-ui-autopush](http://go/perfetto-ui-autopush) for docs.

## /appengine : GAE <> GCS proxy

The Google AppEngine instance that responds to www.perfetto.dev.
It simply passes through the requests to the bucket gs://perfetto.dev .
This should NOT be re-deployed when uploading new docs, as the actual
artifacts live in GCS.

We are using AppEngine for historical reasons, at some point this should
be migrated to a Type 7 Google Cloud Load Balancer, which supports
direct backing by a GCS bucket.
