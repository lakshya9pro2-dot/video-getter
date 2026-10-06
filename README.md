# WPE URL Extractor

A very lightweight, headless C/C++ web page loader and HLS media URL detector built with **WPE WebKit 2.0** for resource-constrained Linux servers (e.g. 512 MB RAM).

## Key Features

- **Embedded Headless WPE WebKit**: Uses `libwpe` and `wpebackend-fdo` SHM offscreen backend with zero desktop or GUI dependencies.
- **Dynamic JavaScript Support**: Executes full JavaScript (including DOM manipulation, fetch, and XHR) to detect dynamically generated media playlists.
- **Zero-Download HLS Detection**: Monitors network responses for `application/vnd.apple.mpegurl` (normalized before semicolon). As soon as the header is detected, the body download is canceled via WebKit's policy decision mechanism.
- **Aggressive Resource Blocking**:
  - Image loading disabled (`webkit_settings_set_auto_load_images`) and blocked via WebKit Content Filters.
  - Web fonts blocked (`font/*`, `.woff`, `.woff2`, `.ttf`, etc.).
  - Built-in lightweight rule filter blocking telemetry, advertising, and tracker domains (DoubleClick, Google Analytics, PopAds, etc.).
- **Immediate Cleanup**: Halts page execution and destroys the WebKit process and objects immediately upon detecting the first matching HLS stream.
- **Memory Optimized**: Uses ephemeral sessions and document-viewer cache models (`WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER`) with zero disk persistence. Tested peak RSS: **~111 MB** (well within 512 MB server limits).
- **HTTP API Mode**: Built-in single-concurrency REST API (`GET /extract?url=...`).

---

## Project Structure

```text
.
├── CMakeLists.txt
├── README.md
├── install.sh
├── run-test.sh
└── src/
    ├── browser.cpp
    ├── browser.hpp
    ├── extractor.cpp
    ├── extractor.hpp
    └── main.cpp
```

---

## 1. Installation

On Debian 13 (Trixie), Ubuntu, or Debian-based distributions:

```bash
sudo apt update
sudo apt install -y \
    libwpewebkit-2.0-dev \
    libwpebackend-fdo-1.0-dev \
    libwpe-1.0-dev \
    pkg-config \
    build-essential \
    cmake \
    libglib2.0-dev \
    libsoup-3.0-dev \
    time \
    curl
```

Or run the automated installer:

```bash
./install.sh
```

---

## 2. Building

### Local Build (CMake)

Build using CMake (using `-j1` to conserve memory during compilation on low-RAM servers):

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j1
```

The resulting binary will be at `build/wpe-url-extractor`.

### Docker Build & Run

Build the minimal, multi-stage Docker image:

```bash
docker build -t wpe-url-extractor .
```

#### Run in HTTP API Server Mode (Default)

```bash
docker run -d -p 8080:8080 --name wpe-extractor wpe-url-extractor
```

Test extraction:
```bash
curl "http://127.0.0.1:8080/extract?url=https://example.com/stream-page"
```

#### Run in CLI Mode

```bash
docker run --rm wpe-url-extractor "https://example.com/stream-page"
```

Extract multiple URLs with JSON output:

```bash
docker run --rm wpe-url-extractor --json "https://site1.com" "https://site2.com"
```

---

## 3. Running

### CLI Mode

#### Single URL Extraction

Extract the first HLS stream URL:

```bash
./build/wpe-url-extractor "https://example.com/stream-page"
```

#### Multi-URL Concurrent Extraction

Pass multiple URLs directly to extract them concurrently:

```bash
./build/wpe-url-extractor "https://site1.com" "https://site2.com" "https://site3.com" --concurrency 4
```

#### Reading URLs from File or Standard Input

Read URLs from a file (one URL per line, lines beginning with `#` ignored):

```bash
./build/wpe-url-extractor --file urls.txt --concurrency 4
```

Or pipe via standard input:

```bash
cat urls.txt | ./build/wpe-url-extractor --stdin --concurrency 4
```

#### Structured JSON Output

Use `--json` to output machine-readable JSON:

```bash
./build/wpe-url-extractor --json "https://site1.com" "https://site2.com"
```

Output:
```json
[
  {
    "input_url": "https://site1.com",
    "success": true,
    "stream_url": "https://site1.com/media/master.m3u8",
    "headers": {
      "Origin": "https://site1.com",
      "Referer": "https://site1.com"
    }
  },
  {
    "input_url": "https://site2.com",
    "success": false,
    "stream_url": null,
    "headers": {
      "Origin": "https://site2.com",
      "Referer": "https://site2.com"
    },
    "error": "Timeout reached without finding HLS URL"
  }
]
```

#### Custom User-Agent

```bash
./build/wpe-url-extractor "https://example.com/stream-page" \
    --user-agent "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
```

Default User-Agent: `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36`

#### Custom Timeout (milliseconds)

```bash
./build/wpe-url-extractor "https://example.com/stream-page" \
    --timeout 10000
```

Default timeout: `15000` ms (15 seconds).

#### Verbose Logging

```bash
./build/wpe-url-extractor "https://example.com/stream-page" -v
```

### Exit Codes

| Code | Meaning |
|---|---|
| `0` | HLS stream URL found (or at least one found in multi-URL mode) |
| `1` | No HLS URL found (timeout reached or page finished) |
| `2` | Invalid input or URL scheme (only `http://` and `https://` allowed) |
| `3` | Browser initialization error |
| `4` | Page or network error (DNS failure, TLS rejection, etc.) |

---

## 4. HTTP API Mode

Run as an asynchronous, concurrent microservice daemon on low-resource servers:

```bash
./build/wpe-url-extractor --server 8080 --concurrency 4
```

### Endpoints

#### 1. Extract Stream (GET)

```http
GET /extract?url=https://example.com/stream-page&timeout=15000
```

Example request:

```bash
curl "http://127.0.0.1:8080/extract?url=http://127.0.0.1:9123/js-fetch"
```

Response on match (HTTP 200):

```json
{
  "success": true,
  "url": "https://example.com/video/master.m3u8",
  "headers": {
    "Origin": "https://example.com",
    "Referer": "https://example.com"
  }
}
```

Response on failure (HTTP 200):

```json
{
  "success": false,
  "url": null,
  "headers": {
    "Origin": "https://example.com",
    "Referer": "https://example.com"
  },
  "error": "Timeout reached without finding HLS URL"
}
```

#### 2. Batch Extract Stream (POST)

Submit batch or single extraction jobs via JSON payload:

```http
POST /extract
Content-Type: application/json

{
  "urls": [
    "https://example.com/page1",
    "https://example.com/page2"
  ],
  "timeout": 15000
}
```

Response (HTTP 200):

```json
{
  "success": true,
  "results": [
    {
      "url": "https://example.com/page1",
      "success": true,
      "stream_url": "https://example.com/video/master.m3u8",
      "headers": {
        "Origin": "https://example.com",
        "Referer": "https://example.com"
      }
    },
    {
      "url": "https://example.com/page2",
      "success": false,
      "stream_url": null,
      "headers": {
        "Origin": "https://example.com",
        "Referer": "https://example.com"
      },
      "error": "Timeout reached without finding HLS URL"
    }
  ]
}
```

#### 3. Health & Concurrency Status (GET)

```http
GET /status
```

Response (HTTP 200):

```json
{
  "status": "ok",
  "active_jobs": 2,
  "queued_jobs": 0,
  "max_concurrency": 4
}
```

---

## 5. Testing

A complete automated test harness is provided in `run-test.sh`. It tests:

1. Dynamic JavaScript `fetch()` request for HLS with query parameters.
2. Dynamic JavaScript `XMLHttpRequest` (XHR) request for HLS.
3. HTTP 302 redirect leading to an HLS playlist.
4. Heavy asset blocking (ensures images and fonts are blocked from loading).
5. Timeout enforcement on pages without HLS streams.
6. Security enforcement (rejection of `file://`, `data://`, etc.).
7. HTTP API server mode.

Run the test suite:

```bash
./run-test.sh
```

---

## 6. Memory Measurement

Measure peak Resident Set Size (RSS) and wall-clock execution time using GNU `/usr/bin/time`:

```bash
/usr/bin/time -v ./build/wpe-url-extractor "https://example.com"
```

Look for:
- `Maximum resident set size (kbytes)`: typically ~110,000 to 115,000 KB (~110 MB).
- `Elapsed (wall clock) time`: typically 0.25s - 0.40s on successful stream discovery.

---

## 7. Troubleshooting

### 1. `wpe: could not load the impl library`
- **Cause**: `libwpe` cannot find backend shared object `libWPEBackend-fdo-1.0.so`.
- **Solution**: The extractor automatically calls `wpe_loader_init("libWPEBackend-fdo-1.0.so")`. Ensure `libwpebackend-fdo-1.0-dev` is installed. You can also explicitly export:
  ```bash
  export WPE_BACKEND_LIBRARY=libWPEBackend-fdo-1.0.so
  ```

### 2. DNS resolution inside container / minimal server
- **Cause**: WebKit's network bubblewrap sandbox may block network namespace access if running without proper Linux capabilities.
- **Solution**: The application sets `WEBKIT_FORCE_SANDBOX=0` by default. You can also run with:
  ```bash
  WEBKIT_FORCE_SANDBOX=0 ./build/wpe-url-extractor "https://example.com"
  ```

### 3. Mesa / GPU DRI warnings on headless servers
- **Cause**: Mesa tries to probe for GPU hardware drivers (`/dev/dri`).
- **Solution**: The application defaults to `LIBGL_ALWAYS_SOFTWARE=1`. Warnings like `failed to get driver name for fd -1` are harmless and handled cleanly by the offscreen SHM backend.

## Logging

The server now emits timestamped logs to stderr, which are visible in Render/container logs.

Log levels are controlled with `LOG_LEVEL`:

- `error` — errors only
- `warn` — warnings and errors
- `info` — normal server/request/extraction lifecycle logs (default)
- `debug` — extra diagnostic logs

Example:

```bash
LOG_LEVEL=debug ./build/wpe-url-extractor --server 8080
```

Each HTTP request receives a request ID, making it easier to follow a request from the API through WPE/WebKit extraction and back to the response.
