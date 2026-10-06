# ==============================================================================
# Production Dockerfile for Render (Debian 13 Trixie based)
# Provides native libwpewebkit-2.0, wpebackend-fdo-1.0, libsoup-3.0 packages.
# ==============================================================================

# ------------------------------------------------------------------------------
# Stage 1: Build environment
# ------------------------------------------------------------------------------
FROM debian:trixie-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    libwpewebkit-2.0-dev \
    libwpebackend-fdo-1.0-dev \
    libwpe-1.0-dev \
    libglib2.0-dev \
    libsoup-3.0-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy CMake configuration and source code
COPY CMakeLists.txt ./
COPY src/ ./src/

# Build optimized Release binary
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# ------------------------------------------------------------------------------
# Stage 2: Minimal runtime image
# ------------------------------------------------------------------------------
FROM debian:trixie-slim AS runner

ENV DEBIAN_FRONTEND=noninteractive
ENV WEBKIT_FORCE_SANDBOX=0
ENV LIBGL_ALWAYS_SOFTWARE=1
ENV WPE_BACKEND_LIBRARY=libWPEBackend-fdo-1.0.so.1

# Install only runtime shared libraries (no compilers/headers)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libwpewebkit-2.0-1 \
    libwpebackend-fdo-1.0-1 \
    libwpe-1.0-1 \
    libsoup-3.0-0 \
    libglib2.0-0t64 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/* \
    && for d in /usr/lib/*-linux-gnu; do \
         if [ -f "$d/libWPEBackend-fdo-1.0.so.1" ] && [ ! -e "$d/libWPEBackend-fdo-1.0.so" ]; then \
           ln -sf "$d/libWPEBackend-fdo-1.0.so.1" "$d/libWPEBackend-fdo-1.0.so"; \
         fi; \
       done

# Run as non-root user for security
RUN useradd -m -u 1000 -s /bin/bash appuser

WORKDIR /home/appuser

# Copy executable and entrypoint script
COPY --from=builder /app/build/wpe-url-extractor /usr/local/bin/wpe-url-extractor
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

USER appuser

# Render sets $PORT dynamically at runtime (default: 8080)
EXPOSE 8080

# Health check verifies the lightweight /health endpoint
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD curl -f http://127.0.0.1:${PORT:-8080}/health || exit 1

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
