#include "extractor.hpp"
#include "browser.hpp"
#include "logger.hpp"

#include <atomic>
#include <chrono>

#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <memory>
#include <glib.h>
#include <glib-unix.h>
#include <libsoup/soup.h>

std::string escape_json(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned char>(c));
                    out += hex;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string normalize_mime(const char* mime_type) {
    if (!mime_type) return "";

    std::string s(mime_type);
    size_t semi = s.find(';');
    if (semi != std::string::npos) {
        s = s.substr(0, semi);
    }

    // Trim whitespace
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);

    // Lowercase
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });

    return s;
}

bool is_hls_mime_type(const char* mime_type) {
    std::string norm = normalize_mime(mime_type);
    return (norm == "application/vnd.apple.mpegurl" || norm == "application/x-mpegurl");
}

// Detect HLS playlists directly from the requested URL. Some servers return
// non-standard MIME types (or application/octet-stream) for .m3u8 playlists,
// so MIME-only detection can miss the stream and wait for the full timeout.
bool is_hls_url(const char* url) {
    if (!url || !*url) return false;

    std::string value(url);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    // Normal playlist URL, including query strings such as .m3u8?token=...
    if (value.find(".m3u8") != std::string::npos) return true;

    // Some providers expose the playlist through a route/query without the
    // conventional extension, e.g. /playlist?format=m3u8.
    return value.find("m3u8") != std::string::npos;
}

std::string get_url_origin(const std::string& url) {
    if (url.empty()) return "";

    GError* error = nullptr;
    GUri* parsed = g_uri_parse(url.c_str(), G_URI_FLAGS_PARSE_RELAXED, &error);
    if (!parsed) {
        if (error) g_clear_error(&error);
        return "";
    }

    const char* scheme = g_uri_get_scheme(parsed);
    const char* host = g_uri_get_host(parsed);
    int port = g_uri_get_port(parsed);

    std::string origin;
    if (scheme && host) {
        origin = std::string(scheme) + "://" + host;
        if (port > 0) {
            bool is_default = (strcmp(scheme, "http") == 0 && port == 80) ||
                              (strcmp(scheme, "https") == 0 && port == 443);
            if (!is_default) {
                origin += ":" + std::to_string(port);
            }
        }
    }

    g_uri_unref(parsed);
    return origin;
}

bool is_valid_http_url(const std::string& url) {
    if (url.empty()) return false;

    // Reject non-http schemes directly
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return false;
    }

    // Validate URI parsing
    GError* error = nullptr;
    GUri* parsed = g_uri_parse(url.c_str(), G_URI_FLAGS_PARSE_RELAXED, &error);
    if (!parsed) {
        if (error) g_clear_error(&error);
        return false;
    }

    const char* scheme = g_uri_get_scheme(parsed);
    const char* host = g_uri_get_host(parsed);

    bool ok = (scheme && (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0) &&
               host && strlen(host) > 0);

    g_uri_unref(parsed);
    return ok;
}

ExtractionResult run_extraction(const ExtractionOptions& options) {
    ExtractionResult result;
    const auto started = std::chrono::steady_clock::now();
    app_log::info("Extraction requested: " + options.url);

    if (!is_valid_http_url(options.url)) {
        result.exit_code = 2; // Invalid input
        result.error_message = "Invalid URL: only http:// and https:// schemes are supported.";
        app_log::warn("Rejected invalid URL: " + options.url);
        return result;
    }

    BrowserEngine browser;
    browser.set_match_callback([&result](const std::string& matched_url) {
        result.found = true;
        result.matched_url = matched_url;
        result.exit_code = 0;
    });

    browser.set_error_callback([&result](int error_code, const std::string& error_msg) {
        if (!result.found) {
            result.exit_code = error_code;
            result.error_message = error_msg;
        }
    });

    if (!browser.init()) {
        if (result.exit_code == 1) {
            result.exit_code = 3; // Browser initialization error
            result.error_message = "Browser initialization failed.";
        }
        return result;
    }

    browser.load_url(options.url, options.user_agent, options.timeout_ms, options.verbose);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    app_log::info("Extraction request completed in " + std::to_string(elapsed) + " ms: " + options.url);
    return result;
}

struct MultiJob {
    std::string url;
    size_t index = 0;
    std::unique_ptr<BrowserEngine> engine;
};

std::vector<MultiExtractionItem> run_multi_extraction(
    const MultiExtractionOptions& options,
    std::function<void(const MultiExtractionItem& item)> progress_cb) {

    size_t total_urls = options.urls.size();
    std::vector<MultiExtractionItem> results(total_urls);
    if (total_urls == 0) return results;

    for (size_t i = 0; i < total_urls; ++i) {
        results[i].url = options.urls[i];
    }

    size_t max_concurrent = std::max<size_t>(1, options.max_concurrency);
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

    struct Context {
        const MultiExtractionOptions& opts;
        std::function<void(const MultiExtractionItem& item)> progress;
        GMainLoop* main_loop;
        std::vector<MultiExtractionItem>& out_results;
        size_t next_idx = 0;
        size_t completed = 0;
        size_t max_concurrency = 1;
        std::vector<std::unique_ptr<MultiJob>> active_jobs;

        void pump() {
            while (active_jobs.size() < max_concurrency && next_idx < opts.urls.size()) {
                size_t idx = next_idx++;
                const std::string& url = opts.urls[idx];

                if (!is_valid_http_url(url)) {
                    out_results[idx].result.found = false;
                    out_results[idx].result.exit_code = 2;
                    out_results[idx].result.error_message = "Invalid URL: only http:// and https:// supported.";
                    if (progress) progress(out_results[idx]);
                    completed++;
                    continue;
                }

                auto job = std::make_unique<MultiJob>();
                job->url = url;
                job->index = idx;
                job->engine = std::make_unique<BrowserEngine>();

                if (!job->engine->init()) {
                    out_results[idx].result.found = false;
                    out_results[idx].result.exit_code = 3;
                    out_results[idx].result.error_message = "Browser initialization failed.";
                    if (progress) progress(out_results[idx]);
                    completed++;
                    continue;
                }

                MultiJob* p_job = job.get();
                active_jobs.push_back(std::move(job));

                p_job->engine->start_async(
                    url, opts.user_agent, opts.timeout_ms, opts.verbose,
                    [this, p_job](bool found, const std::string& matched_url, int exit_code, const std::string& err_msg) {
                        size_t job_idx = p_job->index;
                        out_results[job_idx].result.found = found;
                        out_results[job_idx].result.matched_url = matched_url;
                        out_results[job_idx].result.exit_code = exit_code;
                        out_results[job_idx].result.error_message = err_msg;

                        if (progress) progress(out_results[job_idx]);
                        completed++;

                        g_idle_add(+[](gpointer data) -> gboolean {
                            auto* pair = static_cast<std::pair<Context*, MultiJob*>*>(data);
                            Context* ctx = pair->first;
                            MultiJob* target = pair->second;

                            for (auto it = ctx->active_jobs.begin(); it != ctx->active_jobs.end(); ++it) {
                                if (it->get() == target) {
                                    ctx->active_jobs.erase(it);
                                    break;
                                }
                            }
                            delete pair;

                            if (ctx->completed >= ctx->opts.urls.size()) {
                                if (g_main_loop_is_running(ctx->main_loop)) {
                                    g_main_loop_quit(ctx->main_loop);
                                }
                            } else {
                                ctx->pump();
                            }
                            return G_SOURCE_REMOVE;
                        }, new std::pair<Context*, MultiJob*>(this, p_job));
                    });
            }

            if (completed >= opts.urls.size()) {
                if (g_main_loop_is_running(main_loop)) {
                    g_main_loop_quit(main_loop);
                }
            }
        }
    };

    Context ctx{options, progress_cb, loop, results, 0, 0, max_concurrent, {}};
    ctx.pump();

    if (ctx.completed < total_urls) {
        g_main_loop_run(loop);
    }

    g_main_loop_unref(loop);
    return results;
}

// JSON parsing helper for HTTP API
struct JsonExtractRequest {
    std::string single_url;
    std::vector<std::string> batch_urls;
    std::string user_agent;
    uint32_t timeout_ms = 0;
};

static std::string extract_json_string(const std::string& json, size_t& pos) {
    std::string result;
    bool in_str = false;
    bool esc = false;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (!in_str) {
            if (c == '"') {
                in_str = true;
            }
        } else {
            if (esc) {
                if (c == '"') result += '"';
                else if (c == '\\') result += '\\';
                else if (c == '/') result += '/';
                else if (c == 'b') result += '\b';
                else if (c == 'f') result += '\f';
                else if (c == 'n') result += '\n';
                else if (c == 'r') result += '\r';
                else if (c == 't') result += '\t';
                else result += c;
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                pos++;
                break;
            } else {
                result += c;
            }
        }
    }
    return result;
}

static JsonExtractRequest parse_json_request(const std::string& json) {
    JsonExtractRequest req;
    size_t i = 0;
    while (i < json.size()) {
        char c = json[i];
        if (c == '"') {
            size_t key_pos = i;
            std::string key = extract_json_string(json, key_pos);
            i = key_pos;
            while (i < json.size() && (std::isspace(json[i]) || json[i] == ':')) i++;

            if (key == "urls") {
                while (i < json.size() && json[i] != '[') i++;
                if (i < json.size() && json[i] == '[') {
                    i++;
                    while (i < json.size() && json[i] != ']') {
                        while (i < json.size() && json[i] != '"' && json[i] != ']') i++;
                        if (i < json.size() && json[i] == '"') {
                            size_t str_pos = i;
                            std::string u = extract_json_string(json, str_pos);
                            i = str_pos;
                            if (!u.empty()) req.batch_urls.push_back(u);
                        }
                    }
                    if (i < json.size() && json[i] == ']') i++;
                }
            } else if (key == "url") {
                while (i < json.size() && json[i] != '"') i++;
                if (i < json.size() && json[i] == '"') {
                    size_t str_pos = i;
                    req.single_url = extract_json_string(json, str_pos);
                    i = str_pos;
                }
            } else if (key == "timeout") {
                while (i < json.size() && !std::isdigit(json[i])) i++;
                std::string num;
                while (i < json.size() && std::isdigit(json[i])) num += json[i++];
                if (!num.empty()) req.timeout_ms = std::stoul(num);
            } else if (key == "user_agent") {
                while (i < json.size() && json[i] != '"') i++;
                if (i < json.size() && json[i] == '"') {
                    size_t str_pos = i;
                    req.user_agent = extract_json_string(json, str_pos);
                    i = str_pos;
                }
            }
        } else {
            i++;
        }
    }
    return req;
}

static std::atomic<uint64_t> g_request_id{0};

struct ServerState {
    std::string default_user_agent;
    uint32_t default_timeout_ms = 20000;
    size_t max_concurrency = 4;
    bool verbose = false;

    struct Task {
        uint64_t request_id = 0;
        std::string url;
        std::string user_agent;
        uint32_t timeout_ms;
        bool verbose;
        std::function<void(const ExtractionResult& res)> on_done;
    };

    struct ActiveItem {
        std::unique_ptr<BrowserEngine> engine;
    };

    std::deque<Task> pending_tasks;
    std::vector<std::unique_ptr<ActiveItem>> active_items;

    void enqueue(Task task) {
        pending_tasks.push_back(std::move(task));
        pump();
    }

    void pump() {
        while (active_items.size() < max_concurrency && !pending_tasks.empty()) {
            Task task = std::move(pending_tasks.front());
            pending_tasks.pop_front();
            app_log::info("Starting queued request #" + std::to_string(task.request_id) + " url=" + task.url);

            if (!is_valid_http_url(task.url)) {
                ExtractionResult err;
                err.found = false;
                err.exit_code = 2;
                err.error_message = "Invalid URL: only http:// and https:// schemes are supported.";
                app_log::warn("Request #" + std::to_string(task.request_id) + " rejected: invalid URL");
                task.on_done(err);
                continue;
            }

            auto item = std::make_unique<ActiveItem>();
            item->engine = std::make_unique<BrowserEngine>();
            if (!item->engine->init()) {
                ExtractionResult err;
                err.found = false;
                err.exit_code = 3;
                err.error_message = "Browser initialization failed.";
                task.on_done(err);
                continue;
            }

            ActiveItem* p_item = item.get();
            active_items.push_back(std::move(item));

            auto on_done = std::move(task.on_done);
            p_item->engine->start_async(
                task.url, task.user_agent, task.timeout_ms, task.verbose,
                [this, p_item, on_done](bool found, const std::string& matched_url, int exit_code, const std::string& err_msg) {
                    ExtractionResult res;
                    res.found = found;
                    res.matched_url = matched_url;
                    res.exit_code = exit_code;
                    res.error_message = err_msg;

                    app_log::info("Request completed: found=" + std::string(found ? "true" : "false") + " exit_code=" + std::to_string(exit_code) + (err_msg.empty() ? "" : " error=" + err_msg));
                    on_done(res);

                    g_idle_add(+[](gpointer data) -> gboolean {
                        auto* p = static_cast<std::pair<ServerState*, ActiveItem*>*>(data);
                        ServerState* state = p->first;
                        ActiveItem* target = p->second;

                        for (auto it = state->active_items.begin(); it != state->active_items.end(); ++it) {
                            if (it->get() == target) {
                                state->active_items.erase(it);
                                break;
                            }
                        }
                        delete p;
                        state->pump();
                        return G_SOURCE_REMOVE;
                    }, new std::pair<ServerState*, ActiveItem*>(this, p_item));
                });
        }
    }
};

static void on_api_request(SoupServer* /*server*/,
                           SoupServerMessage* msg,
                           const char* path,
                           GHashTable* query,
                           gpointer user_data) {
    auto* state = static_cast<ServerState*>(user_data);
    const uint64_t request_id = ++g_request_id;
    const char* method = soup_server_message_get_method(msg);
    app_log::info("HTTP request #" + std::to_string(request_id) + " " + (method ? method : "UNKNOWN") + " " + (path ? path : ""));

    // Lightweight Health Check endpoint (zero-cost, no browser execution)
    if (strcmp(path, "/health") == 0) {
        const char* json = "{\"status\":\"ok\"}\n";
        soup_server_message_set_status(msg, SOUP_STATUS_OK, nullptr);
        soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, json, strlen(json));
        app_log::debug("HTTP request #" + std::to_string(request_id) + " health OK");
        return;
    }

    // Detailed Status endpoint
    if (strcmp(path, "/status") == 0) {
        std::string json = "{\n"
            "  \"status\": \"ok\",\n"
            "  \"active_jobs\": " + std::to_string(state->active_items.size()) + ",\n"
            "  \"queued_jobs\": " + std::to_string(state->pending_tasks.size()) + ",\n"
            "  \"max_concurrency\": " + std::to_string(state->max_concurrency) + "\n"
            "}\n";
        soup_server_message_set_status(msg, SOUP_STATUS_OK, nullptr);
        soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, json.c_str(), json.length());
        return;
    }

    if (strcmp(path, "/extract") != 0) {
        soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, nullptr);
        return;
    }

    const char* http_method = method;

    if (strcmp(http_method, "GET") == 0) {
        const char* target_url = query ? static_cast<const char*>(g_hash_table_lookup(query, "url")) : nullptr;
        if (!target_url || !is_valid_http_url(target_url)) {
            const char* err_json = "{\"success\": false, \"url\": null, \"error\": \"Missing or invalid url parameter\"}\n";
            soup_server_message_set_status(msg, SOUP_STATUS_BAD_REQUEST, nullptr);
            soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, err_json, strlen(err_json));
            return;
        }

        uint32_t timeout = state->default_timeout_ms;
        const char* timeout_param = query ? static_cast<const char*>(g_hash_table_lookup(query, "timeout")) : nullptr;
        if (timeout_param) {
            timeout = static_cast<uint32_t>(std::strtoul(timeout_param, nullptr, 10));
        }

        const char* ua_param = query ? static_cast<const char*>(g_hash_table_lookup(query, "user_agent")) : nullptr;
        std::string ua = ua_param ? ua_param : state->default_user_agent;

        std::string origin = get_url_origin(target_url);

        soup_server_message_pause(msg);
        g_object_ref(msg);

        state->enqueue({
            request_id,
            target_url,
            ua,
            timeout,
            state->verbose,
            [msg, origin, request_id](const ExtractionResult& res) {
                std::string resp_json;
                if (res.found) {
                    resp_json = "{\n"
                                "  \"success\": true,\n"
                                "  \"url\": \"" + escape_json(res.matched_url) + "\",\n"
                                "  \"headers\": {\n"
                                "    \"Origin\": \"" + escape_json(origin) + "\",\n"
                                "    \"Referer\": \"" + escape_json(origin) + "\"\n"
                                "  }\n"
                                "}\n";
                } else {
                    resp_json = "{\n"
                                "  \"success\": false,\n"
                                "  \"url\": null,\n"
                                "  \"headers\": {\n"
                                "    \"Origin\": \"" + escape_json(origin) + "\",\n"
                                "    \"Referer\": \"" + escape_json(origin) + "\"\n"
                                "  }";
                    if (!res.error_message.empty()) {
                        resp_json += ",\n  \"error\": \"" + escape_json(res.error_message) + "\"";
                    }
                    resp_json += "\n}\n";
                }

                app_log::info("HTTP request #" + std::to_string(request_id) + " responding success=" + std::string(res.found ? "true" : "false"));
                soup_server_message_set_status(msg, SOUP_STATUS_OK, nullptr);
                soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, resp_json.c_str(), resp_json.length());
                soup_server_message_unpause(msg);
                g_object_unref(msg);
            }
        });
    } else if (strcmp(http_method, "POST") == 0) {
        SoupMessageBody* body = soup_server_message_get_request_body(msg);
        GBytes* bytes = body ? soup_message_body_flatten(body) : nullptr;
        gsize len = 0;
        const char* data = bytes ? static_cast<const char*>(g_bytes_get_data(bytes, &len)) : "";
        std::string payload(data, len);
        if (bytes) g_bytes_unref(bytes);

        JsonExtractRequest req = parse_json_request(payload);

        std::string ua = req.user_agent.empty() ? state->default_user_agent : req.user_agent;
        uint32_t timeout = req.timeout_ms > 0 ? req.timeout_ms : state->default_timeout_ms;

        if (!req.batch_urls.empty()) {
            // Batch extraction mode
            soup_server_message_pause(msg);
            g_object_ref(msg);

            struct BatchHolder {
                SoupServerMessage* msg;
                size_t total = 0;
                size_t completed = 0;
                std::vector<MultiExtractionItem> results;
            };

            auto holder = std::make_shared<BatchHolder>();
            holder->msg = msg;
            holder->total = req.batch_urls.size();
            holder->results.resize(req.batch_urls.size());

            for (size_t i = 0; i < req.batch_urls.size(); ++i) {
                holder->results[i].url = req.batch_urls[i];
                state->enqueue({
                    request_id,
                    req.batch_urls[i],
                    ua,
                    timeout,
                    state->verbose,
                    [holder, i, request_id](const ExtractionResult& res) {
                        holder->results[i].result = res;
                        holder->completed++;

                        if (holder->completed == holder->total) {
                            std::string json = "{\n  \"success\": true,\n  \"results\": [\n";
                            for (size_t j = 0; j < holder->results.size(); ++j) {
                                const auto& r = holder->results[j];
                                std::string item_origin = get_url_origin(r.url);
                                json += "    {\n";
                                json += "      \"url\": \"" + escape_json(r.url) + "\",\n";
                                json += "      \"success\": " + std::string(r.result.found ? "true" : "false") + ",\n";
                                json += "      \"stream_url\": " + (r.result.found ? ("\"" + escape_json(r.result.matched_url) + "\"") : "null") + ",\n";
                                json += "      \"headers\": {\n";
                                json += "        \"Origin\": \"" + escape_json(item_origin) + "\",\n";
                                json += "        \"Referer\": \"" + escape_json(item_origin) + "\"\n";
                                json += "      }";
                                if (!r.result.found && !r.result.error_message.empty()) {
                                    json += ",\n      \"error\": \"" + escape_json(r.result.error_message) + "\"";
                                }
                                json += "\n    }" + std::string(j + 1 < holder->results.size() ? "," : "") + "\n";
                            }
                            json += "  ]\n}\n";

                            app_log::info("HTTP request #" + std::to_string(request_id) + " batch completed items=" + std::to_string(holder->total));
                            soup_server_message_set_status(holder->msg, SOUP_STATUS_OK, nullptr);
                            soup_server_message_set_response(holder->msg, "application/json", SOUP_MEMORY_COPY, json.c_str(), json.length());
                            soup_server_message_unpause(holder->msg);
                            g_object_unref(holder->msg);
                        }
                    }
                });
            }
        } else if (!req.single_url.empty() && is_valid_http_url(req.single_url)) {
            // Single extraction in POST body
            std::string origin = get_url_origin(req.single_url);
            soup_server_message_pause(msg);
            g_object_ref(msg);

            state->enqueue({
                request_id,
                req.single_url,
                ua,
                timeout,
                state->verbose,
                [msg, origin, request_id](const ExtractionResult& res) {
                    std::string resp_json;
                    if (res.found) {
                        resp_json = "{\n"
                                    "  \"success\": true,\n"
                                    "  \"url\": \"" + escape_json(res.matched_url) + "\",\n"
                                    "  \"headers\": {\n"
                                    "    \"Origin\": \"" + escape_json(origin) + "\",\n"
                                    "    \"Referer\": \"" + escape_json(origin) + "\"\n"
                                    "  }\n"
                                    "}\n";
                    } else {
                        resp_json = "{\n"
                                    "  \"success\": false,\n"
                                    "  \"url\": null,\n"
                                    "  \"headers\": {\n"
                                    "    \"Origin\": \"" + escape_json(origin) + "\",\n"
                                    "    \"Referer\": \"" + escape_json(origin) + "\"\n"
                                    "  }";
                        if (!res.error_message.empty()) {
                            resp_json += ",\n  \"error\": \"" + escape_json(res.error_message) + "\"";
                        }
                        resp_json += "\n}\n";
                    }

                    app_log::info("HTTP request #" + std::to_string(request_id) + " POST response success=" + std::string(res.found ? "true" : "false"));
                    soup_server_message_set_status(msg, SOUP_STATUS_OK, nullptr);
                    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, resp_json.c_str(), resp_json.length());
                    soup_server_message_unpause(msg);
                    g_object_unref(msg);
                }
            });
        } else {
            const char* err_json = "{\"success\": false, \"error\": \"Invalid JSON request body. Expected 'url' string or 'urls' array.\"}\n";
            soup_server_message_set_status(msg, SOUP_STATUS_BAD_REQUEST, nullptr);
            soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, err_json, strlen(err_json));
        }
    } else {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED, nullptr);
    }
}

int run_http_server(int port,
                    const std::string& default_user_agent,
                    uint32_t default_timeout_ms,
                    size_t max_concurrency,
                    bool verbose) {
    ServerState state;
    state.default_user_agent = default_user_agent;
    state.default_timeout_ms = default_timeout_ms;
    state.max_concurrency = std::max<size_t>(1, max_concurrency);
    state.verbose = verbose;

    GError* error = nullptr;
    SoupServer* server = soup_server_new(nullptr, nullptr);
    soup_server_add_handler(server, nullptr, on_api_request, &state, nullptr);

    if (!soup_server_listen_all(server, port, static_cast<SoupServerListenOptions>(0), &error)) {
        std::cerr << "Failed to start HTTP server on port " << port << ": "
                  << (error ? error->message : "unknown") << std::endl;
        if (error) g_clear_error(&error);
        g_object_unref(server);
        return 3;
    }

    std::cout << "WPE URL Extractor API server listening on http://0.0.0.0:" << port
              << " (max concurrency: " << state.max_concurrency << ")" << std::endl;
    app_log::info("Server started port=" + std::to_string(port) + " concurrency=" + std::to_string(state.max_concurrency) + " log_level=" + (std::getenv("LOG_LEVEL") ? std::getenv("LOG_LEVEL") : "info"));

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

    // Graceful shutdown on SIGTERM and SIGINT (Docker stop / Ctrl+C)
    g_unix_signal_add(SIGTERM, +[](gpointer user_data) -> gboolean {
        auto* l = static_cast<GMainLoop*>(user_data);
        if (l && g_main_loop_is_running(l)) {
            app_log::info("Received SIGTERM, shutting down gracefully");
            std::cout << "\nReceived SIGTERM, shutting down gracefully..." << std::endl;
            g_main_loop_quit(l);
        }
        return G_SOURCE_REMOVE;
    }, loop);

    g_unix_signal_add(SIGINT, +[](gpointer user_data) -> gboolean {
        auto* l = static_cast<GMainLoop*>(user_data);
        if (l && g_main_loop_is_running(l)) {
            app_log::info("Received SIGINT, shutting down gracefully");
            std::cout << "\nReceived SIGINT, shutting down gracefully..." << std::endl;
            g_main_loop_quit(l);
        }
        return G_SOURCE_REMOVE;
    }, loop);

    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(server);
    return 0;
}
