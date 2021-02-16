# ui.perfetto.dev Cloud scripts

See [go/perfetto-ui-autopush](http://go/perfetto-ui-autopush) for docs on how
this works end-to-end.

## /appengine : GAE <> GCS proxy

The Google AppEngine instance that responds to ui.perfetto.dev.
It simply passes through the requests to the bucket gs://ui.perfetto.dev .
This should NOT be re-deployed when uploading a new version of the ui,
as the actual UI artifacts live in GCS.

We are using AppEngine for historical reasons, at some point this should
be migrated to a Type 7 Google Cloud Load Balancer, which supports
direct backing by a GCS bucket. The only blocker for that is figuring out
a seamless migration strategy for the SSL certificate.

## /builder : Docker container for Google Cloud Build

Contains the Dockerfile to generate the container image which is used by
Google Cloud Build when auto-triggering new ui builds.
Cloud Build invokes the equivalent of:

```bash
docker run gcr.io/perfetto-ui/perfetto-ui-builder \
    ui/release/builder_entrypoint.sh
```

NOTE: the `builder_entrypoint.sh` script is not bundled in the docker container
and is taken from the HEAD if the checked out repo.

To update the container:

```bash
cd infra/ui.perfetto.dev/builder
docker build -t gcr.io/perfetto-ui/perfetto-ui-builder .
docker push gcr.io/perfetto-ui/perfetto-ui-builder .
```
