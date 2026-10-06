#include "browser.hpp"
#include "extractor.hpp"

#include <iostream>
#include <mutex>
#include <unistd.h>
#include <wpe/unstable/fdo-shm.h>

static std::once_flag g_wpe_init_flag;
static bool g_wpe_init_success = false;
static WebKitUserContentFilter* g_cached_filter = nullptr;

static void global_wpe_init() {
    if (!g_getenv("WEBKIT_FORCE_SANDBOX")) {
        g_setenv("WEBKIT_FORCE_SANDBOX", "0", FALSE);
    }
    if (!g_getenv("LIBGL_ALWAYS_SOFTWARE")) {
        g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", FALSE);
    }

    if (!wpe_loader_init("libWPEBackend-fdo-1.0.so") &&
        !wpe_loader_init("libWPEBackend-fdo-1.0.so.1")) {
        return;
    }
    if (!wpe_fdo_initialize_shm()) {
        return;
    }
    g_wpe_init_success = true;
}

static void compile_global_filter() {
    if (g_cached_filter) return;

    const char* rules = "[\n"
        "  {\"trigger\": {\"url-filter\": \".*\", \"resource-type\": [\"image\", \"font\"]}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*doubleclick.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*googletagservices.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*google-analytics.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*googlesyndication.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*adservice.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*adsystem.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*adnxs.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*adroll.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*popads.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*taboola.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*outbrain.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*criteo.*\"}, \"action\": {\"type\": \"block\"}},\n"
        "  {\"trigger\": {\"url-filter\": \".*scorecardresearch.*\"}, \"action\": {\"type\": \"block\"}}\n"
        "]";

    char filter_dir[] = "/tmp/wpe_filters_XXXXXX";
    char* dir = mkdtemp(filter_dir);
    if (!dir) return;

    WebKitUserContentFilterStore* store = webkit_user_content_filter_store_new(dir);
    GBytes* bytes = g_bytes_new_static(rules, strlen(rules));

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    webkit_user_content_filter_store_save(
        store, "blocker", bytes, nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer user_data) {
            GError* error = nullptr;
            g_cached_filter = webkit_user_content_filter_store_save_finish(
                WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &error);
            if (error) {
                g_clear_error(&error);
            }
            g_main_loop_quit(static_cast<GMainLoop*>(user_data));
        },
        loop);

    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    g_bytes_unref(bytes);
    g_object_unref(store);
}

BrowserEngine::BrowserEngine() = default;

BrowserEngine::~BrowserEngine() {
    cleanup();
}

void BrowserEngine::on_export_shm_buffer(void* data, struct wpe_fdo_shm_exported_buffer* buffer) {
    auto* self = static_cast<BrowserEngine*>(data);
    if (self && self->exportable_) {
        wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(self->exportable_, buffer);
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable_);
    }
}

void BrowserEngine::on_export_buffer_resource(void* data, struct wl_resource* buffer_resource) {
    auto* self = static_cast<BrowserEngine*>(data);
    if (self && self->exportable_) {
        wpe_view_backend_exportable_fdo_dispatch_release_buffer(self->exportable_, buffer_resource);
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable_);
    }
}

void BrowserEngine::on_export_dmabuf_resource(void* data, struct wpe_view_backend_exportable_fdo_dmabuf_resource* /*dmabuf_resource*/) {
    auto* self = static_cast<BrowserEngine*>(data);
    if (self && self->exportable_) {
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable_);
    }
}

bool BrowserEngine::init() {
    std::call_once(g_wpe_init_flag, global_wpe_init);
    if (!g_wpe_init_success) {
        if (error_callback_) {
            error_callback_(3, "Failed to initialize WPE FDO backend");
        }
        return false;
    }

    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_buffer_resource = on_export_buffer_resource,
        .export_dmabuf_resource = on_export_dmabuf_resource,
        .export_shm_buffer = on_export_shm_buffer,
        ._wpe_reserved0 = nullptr,
        ._wpe_reserved1 = nullptr
    };

    exportable_ = wpe_view_backend_exportable_fdo_create(&client, this, 800, 600);
    if (!exportable_) {
        if (error_callback_) {
            error_callback_(3, "Failed to create WPE exportable backend");
        }
        return false;
    }

    struct wpe_view_backend* wpe_backend = wpe_view_backend_exportable_fdo_get_view_backend(exportable_);
    view_backend_ = webkit_web_view_backend_new(wpe_backend, (GDestroyNotify)wpe_view_backend_exportable_fdo_destroy, exportable_);

    // Ephemeral network session ensures zero disk caching or persisted sessions
    network_session_ = webkit_network_session_new_ephemeral();
    context_ = webkit_web_context_new();
    webkit_web_context_set_cache_model(context_, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

    // Settings optimized for low RAM
    settings_ = webkit_settings_new();
    webkit_settings_set_auto_load_images(settings_, FALSE);
    webkit_settings_set_enable_webgl(settings_, FALSE);
    webkit_settings_set_enable_media_stream(settings_, FALSE);
    webkit_settings_set_enable_smooth_scrolling(settings_, FALSE);
    webkit_settings_set_enable_javascript(settings_, TRUE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings_, FALSE);

    ucm_ = webkit_user_content_manager_new();
    setup_content_filters();

    // Register script message handler to capture page console logs
    g_signal_connect(ucm_, "script-message-received::consoleLog", G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* value, gpointer user_data) {
        auto* self = static_cast<BrowserEngine*>(user_data);
        if (!self || !self->verbose_ || !value) return;
        char* str = jsc_value_to_string(value);
        if (str) {
            std::cerr << "[JS CONSOLE] " << str << std::endl;
            g_free(str);
        }
    }), this);
    webkit_user_content_manager_register_script_message_handler(ucm_, "consoleLog", nullptr);

    // Inject console interception script
    const char* console_hook_js =
        "(function() {"
        "  function s(t, a) {"
        "    try {"
        "      var m = '[' + t + '] ' + Array.prototype.slice.call(a).map(function(x) {"
        "        try { return typeof x === 'object' ? JSON.stringify(x) : String(x); } catch(e) { return String(x); }"
        "      }).join(' ');"
        "      window.webkit.messageHandlers.consoleLog.postMessage(m);"
        "    } catch(e) {}"
        "  }"
        "  var ol = console.log, ow = console.warn, oe = console.error, oi = console.info;"
        "  console.log = function() { s('LOG', arguments); if (ol) ol.apply(console, arguments); };"
        "  console.warn = function() { s('WARN', arguments); if (ow) ow.apply(console, arguments); };"
        "  console.error = function() { s('ERROR', arguments); if (oe) oe.apply(console, arguments); };"
        "  console.info = function() { s('INFO', arguments); if (oi) oi.apply(console, arguments); };"
        "})();";

    WebKitUserScript* script = webkit_user_script_new(
        console_hook_js,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm_, script);
    webkit_user_script_unref(script);

    web_view_ = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "backend", view_backend_,
        "web-context", context_,
        "network-session", network_session_,
        "settings", settings_,
        "user-content-manager", ucm_,
        nullptr));

    if (!web_view_) {
        if (error_callback_) {
            error_callback_(3, "Failed to create WebKitWebView instance");
        }
        return false;
    }

    g_signal_connect(web_view_, "notify::title", G_CALLBACK(+[](WebKitWebView* wv, GParamSpec*, gpointer user_data) {
        auto* self = static_cast<BrowserEngine*>(user_data);
        if (self && self->verbose_) {
            const char* title = webkit_web_view_get_title(wv);
            if (title && strlen(title) > 0) {
                std::cerr << "[PAGE TITLE] " << title << std::endl;
            }
        }
    }), this);

    g_signal_connect(web_view_, "notify::uri", G_CALLBACK(+[](WebKitWebView* wv, GParamSpec*, gpointer user_data) {
        auto* self = static_cast<BrowserEngine*>(user_data);
        if (self && self->verbose_) {
            const char* uri = webkit_web_view_get_uri(wv);
            if (uri && strlen(uri) > 0) {
                std::cerr << "[PAGE URI CHANGED] " << uri << std::endl;
            }
        }
    }), this);

    g_signal_connect(web_view_, "decide-policy", G_CALLBACK(on_decide_policy), this);
    g_signal_connect(web_view_, "resource-load-started", G_CALLBACK(on_resource_load_started), this);
    g_signal_connect(web_view_, "load-changed", G_CALLBACK(on_load_changed), this);
    g_signal_connect(web_view_, "load-failed", G_CALLBACK(on_load_failed), this);
    g_signal_connect(web_view_, "load-failed-with-tls-errors", G_CALLBACK(on_load_failed_with_tls_errors), this);
    g_signal_connect(web_view_, "web-process-terminated", G_CALLBACK(on_web_process_terminated), this);

    return true;
}

void BrowserEngine::setup_content_filters() {
    if (!ucm_) return;
    if (!g_cached_filter) {
        compile_global_filter();
    }
    if (g_cached_filter) {
        webkit_user_content_manager_add_filter(ucm_, g_cached_filter);
    }
}

void BrowserEngine::finish(bool found, const std::string& matched_url, int exit_code, const std::string& error_msg) {
    if (finished_) return;
    finished_ = true;

    if (timeout_source_id_ > 0) {
        g_source_remove(timeout_source_id_);
        timeout_source_id_ = 0;
    }

    if (web_view_) {
        webkit_web_view_stop_loading(web_view_);
    }

    matched_ = found;
    matched_url_ = matched_url;

    if (match_callback_ && found) {
        match_callback_(matched_url);
    }
    if (error_callback_ && !found) {
        error_callback_(exit_code, error_msg);
    }

    if (completion_callback_) {
        auto cb = std::move(completion_callback_);
        cb(found, matched_url, exit_code, error_msg);
    }

    if (local_loop_ && g_main_loop_is_running(local_loop_)) {
        g_main_loop_quit(local_loop_);
    }
}

void BrowserEngine::check_response(WebKitURIResponse* response, WebKitPolicyDecision* decision_to_ignore, const char* source) {
    if (!response || finished_) return;

    const char* mime = webkit_uri_response_get_mime_type(response);
    const char* uri = webkit_uri_response_get_uri(response);

    if (verbose_ && mime && uri) {
        std::cerr << "[" << source << "] MIME: " << mime << " URI: " << uri << std::endl;
    }

    if (is_hls_mime_type(mime)) {
        // Immediately ignore response so body/media playlist is not downloaded
        if (decision_to_ignore) {
            webkit_policy_decision_ignore(decision_to_ignore);
        }

        finish(true, uri ? uri : "", 0, "");
    }
}

gboolean BrowserEngine::on_decide_policy(WebKitWebView* /*web_view*/, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer user_data) {
    auto* self = static_cast<BrowserEngine*>(user_data);
    if (!self || self->finished_) return FALSE;

    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        auto* nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(nav_decision);
        WebKitURIRequest* req = webkit_navigation_action_get_request(action);
        if (self->verbose_ && req) {
            std::cerr << "[NAVIGATE] Target URI: " << webkit_uri_request_get_uri(req) << std::endl;
        }
    } else if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        auto* rpd = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        WebKitURIResponse* response = webkit_response_policy_decision_get_response(rpd);
        if (response) {
            const char* mime = webkit_uri_response_get_mime_type(response);
            const char* uri = webkit_uri_response_get_uri(response);
            guint status = webkit_uri_response_get_status_code(response);
            if (self->verbose_) {
                std::cerr << "[PAGE RESPONSE] HTTP " << status
                          << " | MIME: " << (mime ? mime : "(none)")
                          << " | URI: " << (uri ? uri : "") << std::endl;
            }
            if (mime) {
                // Secondary check: block images/fonts if reached decide-policy
                if (strncmp(mime, "image/", 6) == 0 || strncmp(mime, "font/", 5) == 0) {
                    if (self->verbose_) {
                        std::cerr << "[BLOCKED ASSET] " << (uri ? uri : "") << std::endl;
                    }
                    webkit_policy_decision_ignore(decision);
                    return TRUE;
                }
            }
            self->check_response(response, decision, "decide-policy");
            if (self->finished_) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

void BrowserEngine::on_resource_load_started(WebKitWebView* /*web_view*/, WebKitWebResource* resource, WebKitURIRequest* request, gpointer user_data) {
    auto* self = static_cast<BrowserEngine*>(user_data);
    if (!self || self->finished_) return;

    if (self->verbose_ && request) {
        const char* uri = webkit_uri_request_get_uri(request);
        const char* method = webkit_uri_request_get_http_method(request);
        std::cerr << "[REQUEST] " << (method ? method : "GET") << " " << (uri ? uri : "") << std::endl;
    }

    g_signal_connect_data(
        resource,
        "finished",
        G_CALLBACK(on_resource_finished),
        self,
        nullptr,
        G_CONNECT_DEFAULT);
}

void BrowserEngine::on_resource_finished(WebKitWebResource* resource, gpointer user_data) {
    auto* self = static_cast<BrowserEngine*>(user_data);
    if (!self || self->finished_) return;

    WebKitURIResponse* response = webkit_web_resource_get_response(resource);
    if (self->verbose_ && response) {
        guint status = webkit_uri_response_get_status_code(response);
        const char* mime = webkit_uri_response_get_mime_type(response);
        const char* uri = webkit_uri_response_get_uri(response);
        guint64 len = webkit_uri_response_get_content_length(response);
        std::cerr << "[RESPONSE] HTTP " << status
                  << " | " << (mime ? mime : "unknown/mime")
                  << " | " << len << " B"
                  << " | " << (uri ? uri : "") << std::endl;
    }
    self->check_response(response, nullptr, "resource-finished");
}

void BrowserEngine::on_load_changed(WebKitWebView* web_view, WebKitLoadEvent load_event, gpointer user_data) {
    auto* self = static_cast<BrowserEngine*>(user_data);
    if (!self || self->finished_) return;

    if (self->verbose_) {
        const char* uri = webkit_web_view_get_uri(web_view);
        const char* event_name = "UNKNOWN";
        switch (load_event) {
            case WEBKIT_LOAD_STARTED: event_name = "STARTED"; break;
            case WEBKIT_LOAD_REDIRECTED: event_name = "REDIRECTED"; break;
            case WEBKIT_LOAD_COMMITTED: event_name = "COMMITTED"; break;
            case WEBKIT_LOAD_FINISHED: event_name = "FINISHED"; break;
        }
        std::cerr << "[PAGE LOAD " << event_name << "] " << (uri ? uri : "") << std::endl;
    }
}

gboolean BrowserEngine::on_load_failed(WebKitWebView* /*web_view*/, WebKitLoadEvent /*load_event*/, const gchar* failing_uri, GError* error, gpointer user_data) {
    auto* self = static_cast<BrowserEngine*>(user_data);
    if (!self || self->finished_) return FALSE;

    if (self->verbose_) {
        std::cerr << "Load failed on " << (failing_uri ? failing_uri : "unknown")
                  << ": " << (error ? error->message : "unknown") << std::endl;
    }

    self->finish(false, "", 4, error ? error->message : "Page or network error");
    return TRUE;
}

gboolean BrowserEngine::on_load_failed_with_tls_errors(WebKitWebView* /*web_view*/, const gchar* failing_uri, GTlsCertificate* /*certificate*/, GTlsCertificateFlags errors, gpointer user_data) {
    auto* self = static_cast<BrowserEngine*>(user_data);
    if (!self || self->finished_) return FALSE;

    if (self->verbose_) {
        std::cerr << "TLS certificate error (flags " << errors << ") on "
                  << (failing_uri ? failing_uri : "unknown") << std::endl;
    }

    self->finish(false, "", 4, "TLS certificate verification failed");
    return TRUE;
}

void BrowserEngine::on_web_process_terminated(WebKitWebView* /*web_view*/, WebKitWebProcessTerminationReason reason, gpointer user_data) {
    auto* self = static_cast<BrowserEngine*>(user_data);
    if (!self || self->finished_) return;

    self->finish(false, "", 3, "Web process terminated unexpectedly (reason " + std::to_string(reason) + ")");
}

gboolean BrowserEngine::on_timeout_cb(gpointer user_data) {
    auto* self = static_cast<BrowserEngine*>(user_data);
    if (!self) return G_SOURCE_REMOVE;

    self->timeout_source_id_ = 0;
    self->finish(false, "", 1, "Timeout reached without finding HLS URL");
    return G_SOURCE_REMOVE;
}

bool BrowserEngine::start_async(const std::string& url,
                                const std::string& user_agent,
                                uint32_t timeout_ms,
                                bool verbose,
                                CompletionCallback completion_cb) {
    verbose_ = verbose;
    matched_ = false;
    finished_ = false;
    matched_url_.clear();
    completion_callback_ = std::move(completion_cb);

    if (!web_view_) {
        finish(false, "", 3, "Browser engine not initialized");
        return false;
    }

    if (!user_agent.empty() && settings_) {
        webkit_settings_set_user_agent(settings_, user_agent.c_str());
    }

    if (timeout_ms > 0) {
        timeout_source_id_ = g_timeout_add(timeout_ms, on_timeout_cb, this);
    }

    if (verbose_) {
        std::cerr << "Navigating to: " << url << std::endl;
    }

    webkit_web_view_load_uri(web_view_, url.c_str());
    return true;
}

bool BrowserEngine::load_url(const std::string& url,
                             const std::string& user_agent,
                             uint32_t timeout_ms,
                             bool verbose) {
    local_loop_ = g_main_loop_new(nullptr, FALSE);

    if (!start_async(url, user_agent, timeout_ms, verbose, nullptr)) {
        if (local_loop_) {
            g_main_loop_unref(local_loop_);
            local_loop_ = nullptr;
        }
        return false;
    }

    if (!finished_) {
        g_main_loop_run(local_loop_);
    }

    if (local_loop_) {
        g_main_loop_unref(local_loop_);
        local_loop_ = nullptr;
    }

    return matched_;
}

void BrowserEngine::stop() {
    finish(false, "", 1, "Stopped");
}

void BrowserEngine::cleanup() {
    stop();

    if (web_view_) {
        g_signal_handlers_disconnect_by_data(web_view_, this);
        g_object_unref(web_view_);
        web_view_ = nullptr;
    }

    if (settings_) {
        g_object_unref(settings_);
        settings_ = nullptr;
    }

    if (ucm_) {
        webkit_user_content_manager_unregister_script_message_handler(ucm_, "consoleLog", nullptr);
        g_object_unref(ucm_);
        ucm_ = nullptr;
    }

    if (context_) {
        g_object_unref(context_);
        context_ = nullptr;
    }

    if (network_session_) {
        g_object_unref(network_session_);
        network_session_ = nullptr;
    }

    exportable_ = nullptr;
    view_backend_ = nullptr;

    if (local_loop_) {
        if (g_main_loop_is_running(local_loop_)) {
            g_main_loop_quit(local_loop_);
        }
        g_main_loop_unref(local_loop_);
        local_loop_ = nullptr;
    }
}
