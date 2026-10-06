#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/build/wpe-url-extractor"
PORT=9123

if [ ! -f "$BIN" ]; then
    echo "Executable not found at $BIN. Building..."
    mkdir -p "$SCRIPT_DIR/build"
    cd "$SCRIPT_DIR/build"
    cmake ..
    cmake --build . -j1
fi

echo "========================================="
echo " Starting WPE URL Extractor Test Suite   "
echo "========================================="

# Start comprehensive python test fixture server
python3 - << EOF &
import http.server
import socketserver
import time

PORT = $PORT

class TestHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        # Suppress noisy logs
        pass

    def do_GET(self):
        # 1. Normal HTML without HLS
        if self.path == '/normal':
            body = b"<!DOCTYPE html><html><body><h1>Hello World</h1></body></html>"
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        # 2. Page with dynamic JS fetch requesting HLS with query params
        elif self.path == '/js-fetch':
            body = b"""<!DOCTYPE html><html><body><script>
                fetch('/media/playlist.m3u8?token=xyz123&exp=99999');
            </script></body></html>"""
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        # 3. Page with XMLHttpRequest requesting HLS
        elif self.path == '/js-xhr':
            body = b"""<!DOCTYPE html><html><body><script>
                var xhr = new XMLHttpRequest();
                xhr.open('GET', '/media/xhr_stream.m3u8');
                xhr.send();
            </script></body></html>"""
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        # 4. Redirecting to HLS playlist
        elif self.path == '/redirect-to-hls':
            self.send_response(302)
            self.send_header('Location', f'http://127.0.0.1:{PORT}/media/redirected_final.m3u8')
            self.end_headers()

        # 5. Image & font heavy page with HLS
        elif self.path == '/heavy-assets':
            body = b"""<!DOCTYPE html><html><head>
                <link rel="stylesheet" href="/assets/font.woff2">
            </head><body>
                <img src="/assets/img1.png">
                <img src="/assets/img2.jpg">
                <script>fetch('/media/assets_test.m3u8');</script>
            </body></html>"""
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        # HLS endpoints
        elif '/media/' in self.path:
            body = b"#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:10.0,\nseg1.ts\n"
            self.send_response(200)
            self.send_header('Content-Type', 'application/vnd.apple.mpegurl; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        # Assets endpoints
        elif '/assets/' in self.path:
            self.send_response(200)
            self.send_header('Content-Type', 'image/png' if '.png' in self.path else 'font/woff2')
            self.send_header('Content-Length', '10')
            self.end_headers()
            self.wfile.write(b"0123456789")

        else:
            self.send_error(404)

socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("", PORT), TestHandler) as httpd:
    httpd.serve_forever()
EOF
SERVER_PID=$!
trap "kill -9 $SERVER_PID 2>/dev/null || true" EXIT
sleep 1

run_case() {
    local name="$1"
    local url="$2"
    local expected_code="$3"
    local extra_args="${4:-}"

    echo "-----------------------------------------"
    echo "TEST: $name"
    echo "URL: $url"

    local tmpout=$(mktemp)
    local tmptime=$(mktemp)

    /usr/bin/time -v "$BIN" $extra_args "$url" > "$tmpout" 2> "$tmptime" || CODE=$?
    CODE=${CODE:-0}

    local output=$(cat "$tmpout")
    local peak_rss=$(grep "Maximum resident set size" "$tmptime" | awk '{print $NF}')
    local elapsed=$(grep "Elapsed (wall clock) time" "$tmptime" | awk '{print $NF}')

    echo "Result Output: $output"
    echo "Exit Code:     $CODE (Expected: $expected_code)"
    echo "Peak RSS:      ${peak_rss} KB (~$(( peak_rss / 1024 )) MB)"
    echo "Elapsed Time:  $elapsed"

    if [ "$CODE" -eq "$expected_code" ]; then
        echo "STATUS: PASS"
    else
        echo "STATUS: FAIL (unexpected exit code)"
        exit 1
    fi
    rm -f "$tmpout" "$tmptime"
    unset CODE
}

# Test 1: JavaScript fetch HLS with query parameters
run_case "JS Fetch HLS with query params" "http://127.0.0.1:$PORT/js-fetch" 0

# Test 2: JavaScript XMLHttpRequest HLS
run_case "JS XHR Request HLS" "http://127.0.0.1:$PORT/js-xhr" 0

# Test 3: HTTP Redirect leading to HLS URL
run_case "HTTP Redirect to HLS" "http://127.0.0.1:$PORT/redirect-to-hls" 0

# Test 4: Image and Font heavy page (verifying blocking & fast HLS detection)
run_case "Heavy assets page with image/font blocking" "http://127.0.0.1:$PORT/heavy-assets" 0

# Test 5: Page without HLS (Timeout expected)
run_case "No HLS page (Timeout 3s)" "http://127.0.0.1:$PORT/normal" 1 "--timeout 3000"

# Test 6: Security: Reject forbidden URL scheme (file://)
run_case "Security reject file:// URI" "file:///etc/passwd" 2

# Test 7: HTTP API mode
echo "-----------------------------------------"
echo "TEST: HTTP API server mode (/extract?url=...)"
API_PORT=9124
"$BIN" --server $API_PORT --concurrency 4 &
API_PID=$!
sleep 1

API_RESP=$(curl -s "http://127.0.0.1:$API_PORT/extract?url=http://127.0.0.1:$PORT/js-fetch")
echo "API Success Response: $API_RESP"
if echo "$API_RESP" | grep -q '"success": true' && \
   echo "$API_RESP" | grep -q '"Origin": "http://127.0.0.1:' && \
   echo "$API_RESP" | grep -q '"Referer": "http://127.0.0.1:'; then
    echo "API Test 1: PASS"
else
    echo "API Test 1: FAIL"
    kill -9 $API_PID || true
    exit 1
fi

API_FAIL_RESP=$(curl -s "http://127.0.0.1:$API_PORT/extract?url=http://127.0.0.1:$PORT/normal&timeout=2000")
echo "API No-HLS Response: $API_FAIL_RESP"
if echo "$API_FAIL_RESP" | grep -q '"success": false' && \
   echo "$API_FAIL_RESP" | grep -q '"Origin": "http://127.0.0.1:'; then
    echo "API Test 2: PASS"
else
    echo "API Test 2: FAIL"
    kill -9 $API_PID || true
    exit 1
fi

# Test 8: CLI Multi-URL concurrent execution
echo "-----------------------------------------"
echo "TEST: CLI Multi-URL Concurrent Extraction"
MULTI_CLI_OUT=$("$BIN" -c 2 "http://127.0.0.1:$PORT/js-fetch" "http://127.0.0.1:$PORT/js-xhr" "http://127.0.0.1:$PORT/redirect-to-hls")
echo "$MULTI_CLI_OUT"
if echo "$MULTI_CLI_OUT" | grep -q "Done. Found: 3/3"; then
    echo "CLI Multi-URL Test: PASS"
else
    echo "CLI Multi-URL Test: FAIL"
    kill -9 $API_PID || true
    exit 1
fi

# Test 9: CLI Multi-URL JSON mode
echo "-----------------------------------------"
echo "TEST: CLI Multi-URL JSON output"
JSON_OUT=$("$BIN" --json --timeout 2000 "http://127.0.0.1:$PORT/js-fetch" "http://127.0.0.1:$PORT/normal")
echo "$JSON_OUT"
if echo "$JSON_OUT" | grep -q '"input_url":' && \
   echo "$JSON_OUT" | grep -q '"success": true' && \
   echo "$JSON_OUT" | grep -q '"Origin": "http://127.0.0.1:' && \
   echo "$JSON_OUT" | grep -q '"Referer": "http://127.0.0.1:'; then
    echo "CLI JSON Test: PASS"
else
    echo "CLI JSON Test: FAIL"
    kill -9 $API_PID || true
    exit 1
fi

# Test 10: HTTP API concurrent parallel requests
echo "-----------------------------------------"
echo "TEST: HTTP API Concurrent Parallel Requests"
RESP1_FILE=$(mktemp)
RESP2_FILE=$(mktemp)
RESP3_FILE=$(mktemp)

curl -s "http://127.0.0.1:$API_PORT/extract?url=http://127.0.0.1:$PORT/js-fetch" > "$RESP1_FILE" &
PID1=$!
curl -s "http://127.0.0.1:$API_PORT/extract?url=http://127.0.0.1:$PORT/js-xhr" > "$RESP2_FILE" &
PID2=$!
curl -s "http://127.0.0.1:$API_PORT/extract?url=http://127.0.0.1:$PORT/redirect-to-hls" > "$RESP3_FILE" &
PID3=$!

wait $PID1
wait $PID2
wait $PID3

echo "Parallel Resp 1: $(cat $RESP1_FILE)"
echo "Parallel Resp 2: $(cat $RESP2_FILE)"
echo "Parallel Resp 3: $(cat $RESP3_FILE)"

if grep -q '"success": true' "$RESP1_FILE" && \
   grep -q '"success": true' "$RESP2_FILE" && \
   grep -q '"success": true' "$RESP3_FILE"; then
    echo "HTTP API Concurrent Requests: PASS"
else
    echo "HTTP API Concurrent Requests: FAIL"
    rm -f "$RESP1_FILE" "$RESP2_FILE" "$RESP3_FILE"
    kill -9 $API_PID || true
    exit 1
fi
rm -f "$RESP1_FILE" "$RESP2_FILE" "$RESP3_FILE"

# Test 11: HTTP API Batch POST extraction
echo "-----------------------------------------"
echo "TEST: HTTP API Batch POST extraction (/extract)"
BATCH_RESP=$(curl -s -X POST "http://127.0.0.1:$API_PORT/extract" \
  -H "Content-Type: application/json" \
  -d '{"urls": ["http://127.0.0.1:'"$PORT"'/js-fetch", "http://127.0.0.1:'"$PORT"'/js-xhr"]}')
echo "Batch POST Response: $BATCH_RESP"
if echo "$BATCH_RESP" | grep -q '"results":' && echo "$BATCH_RESP" | grep -q 'playlist.m3u8'; then
    echo "HTTP API Batch POST Test: PASS"
else
    echo "HTTP API Batch POST Test: FAIL"
    kill -9 $API_PID || true
    exit 1
fi

# Test 12: HTTP API Status endpoint
echo "-----------------------------------------"
echo "TEST: HTTP API Status endpoint (/status)"
STATUS_RESP=$(curl -s "http://127.0.0.1:$API_PORT/status")
echo "Status Response: $STATUS_RESP"
if echo "$STATUS_RESP" | grep -q '"status": "ok"'; then
    echo "HTTP API Status Test: PASS"
else
    echo "HTTP API Status Test: FAIL"
    kill -9 $API_PID || true
    exit 1
fi

# Test 13: Lightweight Health Check endpoint
echo "-----------------------------------------"
echo "TEST: HTTP API Health Check endpoint (/health)"
HEALTH_RESP=$(curl -s "http://127.0.0.1:$API_PORT/health")
echo "Health Response: $HEALTH_RESP"
if [ "$HEALTH_RESP" = '{"status":"ok"}' ]; then
    echo "HTTP API Health Test: PASS"
else
    echo "HTTP API Health Test: FAIL"
    kill -9 $API_PID || true
    exit 1
fi

kill -9 $API_PID 2>/dev/null || true

echo "========================================="
echo " ALL TESTS PASSED SUCCESSFULLY!          "
echo "========================================="

