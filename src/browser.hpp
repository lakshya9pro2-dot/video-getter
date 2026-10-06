#pragma once

#include <string>
#include <functional>
#include <glib.h>
#include <wpe/webkit.h>
#include <wpe/wpe.h>
#include <wpe/fdo.h>

class BrowserEngine {
public:
    using MatchCallback = std::function<void(const std::string& matched_url)>;
    using ErrorCallback = std::function<void(int error_code, const std::string& error_msg)>;
    using CompletionCallback = std::function<void(bool found, const std::string& matched_url, int exit_code, const std::string& error_msg)>;

    BrowserEngine();
    ~BrowserEngine();

    // Disable copy
    BrowserEngine(const BrowserEngine&) = delete;
    BrowserEngine& operator=(const BrowserEngine&) = delete;

    bool init();

    void set_match_callback(MatchCallback cb) { match_callback_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

    // Asynchronously loads URL on the current GLib context and invokes cb when finished
    bool start_async(const std::string& url,
                     const std::string& user_agent,
                     uint32_t timeout_ms,
                     bool verbose,
                     CompletionCallback cb);

    // Synchronous load: creates a temporary GMainLoop and blocks until done
    bool load_url(const std::string& url,
                  const std::string& user_agent,
                  uint32_t timeout_ms,
                  bool verbose);

    void stop();

    bool is_finished() const { return finished_; }
    bool is_matched() const { return matched_; }
    const std::string& get_matched_url() const { return matched_url_; }

private:
    void cleanup();
    void setup_content_filters();
    void check_response(WebKitURIResponse* response, WebKitPolicyDecision* decision_to_ignore, const char* source);
    void finish(bool found, const std::string& matched_url, int exit_code, const std::string& error_msg);

    static void on_export_shm_buffer(void* data, struct wpe_fdo_shm_exported_buffer* buffer);
    static void on_export_buffer_resource(void* data, struct wl_resource* buffer_resource);
    static void on_export_dmabuf_resource(void* data, struct wpe_view_backend_exportable_fdo_dmabuf_resource* dmabuf_resource);

    static gboolean on_decide_policy(WebKitWebView* web_view, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer user_data);
    static void on_resource_load_started(WebKitWebView* web_view, WebKitWebResource* resource, WebKitURIRequest* request, gpointer user_data);
    static void on_resource_finished(WebKitWebResource* resource, gpointer user_data);
    static void on_load_changed(WebKitWebView* web_view, WebKitLoadEvent load_event, gpointer user_data);
    static gboolean on_load_failed(WebKitWebView* web_view, WebKitLoadEvent load_event, const gchar* failing_uri, GError* error, gpointer user_data);
    static gboolean on_load_failed_with_tls_errors(WebKitWebView* web_view, const gchar* failing_uri, GTlsCertificate* certificate, GTlsCertificateFlags errors, gpointer user_data);
    static void on_web_process_terminated(WebKitWebView* web_view, WebKitWebProcessTerminationReason reason, gpointer user_data);
    static gboolean on_timeout_cb(gpointer user_data);

    struct wpe_view_backend_exportable_fdo* exportable_ = nullptr;
    WebKitWebViewBackend* view_backend_ = nullptr;
    WebKitNetworkSession* network_session_ = nullptr;
    WebKitWebContext* context_ = nullptr;
    WebKitSettings* settings_ = nullptr;
    WebKitUserContentManager* ucm_ = nullptr;
    WebKitWebView* web_view_ = nullptr;
    GMainLoop* local_loop_ = nullptr;
    guint timeout_source_id_ = 0;

    MatchCallback match_callback_;
    ErrorCallback error_callback_;
    CompletionCallback completion_callback_;

    bool matched_ = false;
    bool finished_ = false;
    bool verbose_ = false;
    std::string matched_url_;
};
