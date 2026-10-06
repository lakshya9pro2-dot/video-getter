# ==============================================================================
# Multi-stage Dockerfile for WPE URL Extractor
# Produces a minimal, secure, low-RAM headless container image.
# ==============================================================================

# ------------------------------------------------------------------------------
# Stage 1: Build environment
# ------------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

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

# Copy build files and source code
COPY CMakeLists.txt ./
COPY src/ ./src/

# Compile optimized release binary
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# ------------------------------------------------------------------------------
# Stage 2: Minimal runtime image
# ------------------------------------------------------------------------------
FROM ubuntu:24.04 AS runner

ENV DEBIAN_FRONTEND=noninteractive
ENV WEBKIT_FORCE_SANDBOX=0
ENV LIBGL_ALWAYS_SOFTWARE=1

# Install only shared libraries needed at runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libwpewebkit-2.0-1 \
    libwpebackend-fdo-1.0-1 \
    libwpe-1.0-1 \
    libsoup-3.0-0 \
    libglib2.0-0t64 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Run as non-root user for security
RUN useradd -m -u 1000 -s /bin/bash appuser

WORKDIR /home/appuser

# Copy executable from builder stage
COPY --from=builder /app/build/wpe-url-extractor /usr/local/bin/wpe-url-extractor

USER appuser

EXPOSE 8080

# Health check verifies the HTTP server status endpoint
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD curl -f http://127.0.0.1:8080/status || exit 1

ENTRYPOINT ["/usr/local/bin/wpe-url-extractor"]
CMD ["--server", "8080", "--concurrency", "4"]
