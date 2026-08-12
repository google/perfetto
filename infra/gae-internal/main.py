import posixpath

from flask import Flask, Response, abort
from flask_cors import CORS
from google.cloud import storage

app = Flask(__name__)
CORS(
    app,
    origins=["https://ui.perfetto.dev", "http://localhost:10000"],
    supports_credentials=True)

BUCKET_NAME = "perfetto-ui-internal"
BASE_PREFIX = "extension-server-v1"

gcs_client = storage.Client()
bucket = gcs_client.bucket(BUCKET_NAME)


@app.route("/", defaults={"path": ""})
@app.route("/<path:path>")
def serve(path):
  # Normalize and reject any path traversal attempts.
  normalized = posixpath.normpath(path) if path else ""
  if normalized.startswith(
      "..") or "/../" in normalized or normalized.startswith("/"):
    abort(403)

  blob_path = f"{BASE_PREFIX}/{normalized}" if normalized else BASE_PREFIX + "/"

  blob = bucket.blob(blob_path)
  if not blob.exists():
    abort(404)

  data = blob.download_as_bytes()
  content_type = blob.content_type or "application/octet-stream"
  resp = Response(data, content_type=content_type)
  # Cache in the browser but never in shared/middleware caches (CDN, GFE). The
  # CORS response varies per-origin (Access-Control-Allow-Origin reflects the
  # request Origin), and shared caches here don't key on Vary: Origin, so a
  # cached response for one origin would otherwise be served to another and
  # break CORS. "private" keeps it out of those caches; the browser still
  # caches per-origin.
  resp.headers["Cache-Control"] = "private, max-age=3600"
  return resp


if __name__ == "__main__":
  app.run(host="127.0.0.1", port=8080)
