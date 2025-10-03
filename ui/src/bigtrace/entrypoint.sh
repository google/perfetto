#!/bin/sh
# Start Gunicorn in the background
gunicorn --bind 0.0.0.0:8080 --workers 2 --threads 8 main:app &

# Start Nginx in the foreground
# The 'exec' command is crucial as it replaces the shell process with nginx,
# allowing nginx to receive signals directly from Docker for graceful shutdowns.
exec nginx -g "daemon off;"
