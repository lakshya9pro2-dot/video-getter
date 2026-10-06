#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

struct ExtractionOptions {
    std::string url;
    std::string user_agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36";
    uint32_t timeout_ms = 20000;
    bool verbose = false;
};

struct ExtractionResult {
    bool found = false;
    std::string matched_url;
    int exit_code = 1; // 0=found, 1=not found, 2=invalid input, 3=browser init error, 4=page/network error
    std::string error_message;
};

struct MultiExtractionOptions {
    std::vector<std::string> urls;
    std::string user_agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36";
    uint32_t timeout_ms = 20000;
    size_t max_concurrency = 4;
    bool verbose = false;
    bool output_json = false;
};

struct MultiExtractionItem {
    std::string url;
    ExtractionResult result;
};

// URL validation (only http and https allowed)
bool is_valid_http_url(const std::string& url);

// Normalizes MIME type (lowercase, strip parameters after ';') and checks for HLS
bool is_hls_mime_type(const char* mime_type);
bool is_hls_url(const char* url);
std::string normalize_mime(const char* mime_type);

// Helper to escape strings in JSON output
std::string escape_json(const std::string& input);

// Extracts origin (scheme://host[:port]) from URL
std::string get_url_origin(const std::string& url);

// Runs a single-shot extraction job
ExtractionResult run_extraction(const ExtractionOptions& options);

// Runs multi-extraction concurrently across multiple URLs
std::vector<MultiExtractionItem> run_multi_extraction(
    const MultiExtractionOptions& options,
    std::function<void(const MultiExtractionItem& item)> progress_cb = nullptr);

// Runs concurrent HTTP API server mode
int run_http_server(int port,
                    const std::string& default_user_agent,
                    uint32_t default_timeout_ms,
                    size_t max_concurrency = 4,
                    bool verbose = false);
