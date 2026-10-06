#!/bin/sh
set -e

# Default environment variables for Render and container environments
export PORT="${PORT:-8080}"
export CONCURRENCY="${CONCURRENCY:-2}"
export LOG_LEVEL="${LOG_LEVEL:-info}"
export WEBKIT_FORCE_SANDBOX="${WEBKIT_FORCE_SANDBOX:-0}"
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
export WPE_BACKEND_LIBRARY="${WPE_BACKEND_LIBRARY:-libWPEBackend-fdo-1.0.so.1}"

# If no arguments provided, launch in HTTP API server mode using $PORT
if [ "$#" -eq 0 ]; then
    echo "Starting WPE URL Extractor HTTP server on port $PORT (concurrency: $CONCURRENCY, log level: $LOG_LEVEL)..."
    exec /usr/local/bin/wpe-url-extractor --server "$PORT" --concurrency "$CONCURRENCY"
fi

# If arguments start with a flag (e.g. -v or --timeout), pass them to server mode
if [ "${1#-}" != "$1" ]; then
    echo "Starting WPE URL Extractor HTTP server on port $PORT with extra options..."
    exec /usr/local/bin/wpe-url-extractor --server "$PORT" --concurrency "$CONCURRENCY" "$@"
fi

# Otherwise, execute user command or single-shot extraction CLI
exec /usr/local/bin/wpe-url-extractor "$@"
