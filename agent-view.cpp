#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <string>
#include <cstdlib>
#include <fstream>
#include <ctime>

// Globals for console
GtkWidget* console_view;
GtkTextBuffer* console_buffer;
bool console_visible = false;
GtkWidget* console_scroll;

void log_history(const char* url, const char* title) {
    std::ofstream file(std::string(getenv("HOME")) + "/.agent_history", std::ios::app);
    if (file.is_open()) {
        time_t now = time(nullptr);
        file << now << "|" << url << "|" << (title ? title : "") << "\n";
    }
}

static void on_load_changed(WebKitWebView* webview, WebKitLoadEvent event, gpointer data) {
    if (event == WEBKIT_LOAD_FINISHED) {
        const char* url = webkit_web_view_get_uri(webview);
        const char* title = webkit_web_view_get_title(webview);
        log_history(url, title);
    }
}

static void on_back(GtkWidget* btn, WebKitWebView* webview) {
    if (webkit_web_view_can_go_back(webview))
        webkit_web_view_go_back(webview);
}

static void on_forward(GtkWidget* btn, WebKitWebView* webview) {
    if (webkit_web_view_can_go_forward(webview))
        webkit_web_view_go_forward(webview);
}

// Toggle console visibility
static void on_toggle_console(GtkWidget* btn, gpointer data) {
    console_visible = !console_visible;
    if (console_visible) {
        gtk_widget_show(console_scroll);
    } else {
        gtk_widget_hide(console_scroll);
    }
}

// Handle navigation policy - intercept new window requests
static gboolean on_decide_policy(WebKitWebView* webview,
                                  WebKitPolicyDecision* decision,
                                  WebKitPolicyDecisionType type,
                                  gpointer data) {
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationPolicyDecision* nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(nav_decision);
        if (action) {
            WebKitURIRequest* request = webkit_navigation_action_get_request(action);
            if (request) {
                const char* uri = webkit_uri_request_get_uri(request);
                if (uri) {
                    // Load in same window instead of new window
                    webkit_web_view_load_uri(webview, uri);
                }
            }
        }
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }
    return FALSE;
}

// Handle permission requests
static gboolean on_permission_request(WebKitWebView* webview,
                                       WebKitPermissionRequest* request,
                                       gpointer data) {
    webkit_permission_request_allow(request);
    return TRUE;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: agent-view <html-file-or-url>\n");
        return 1;
    }

    setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);

    gtk_init(&argc, &argv);

    // Persistent storage
    std::string data_dir = std::string(getenv("HOME")) + "/.agent_browser";
    std::string cache_dir = data_dir + "/cache";

    WebKitWebsiteDataManager* data_manager = webkit_website_data_manager_new(
        "base-data-directory", data_dir.c_str(),
        "base-cache-directory", cache_dir.c_str(),
        NULL
    );

    WebKitWebContext* context = webkit_web_context_new_with_website_data_manager(data_manager);

    // Cookies
    WebKitCookieManager* cookie_manager = webkit_web_context_get_cookie_manager(context);
    std::string cookie_file = data_dir + "/cookies.sqlite";
    webkit_cookie_manager_set_persistent_storage(
        cookie_manager,
        cookie_file.c_str(),
        WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE
    );
    webkit_cookie_manager_set_accept_policy(cookie_manager, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);

    // Window
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Agent Browser");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 700);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    // Main layout
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Toolbar
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 5);

    GtkWidget* back_btn = gtk_button_new_with_label("←");
    GtkWidget* forward_btn = gtk_button_new_with_label("→");
    GtkWidget* console_btn = gtk_button_new_with_label("Console");

    gtk_box_pack_start(GTK_BOX(toolbar), back_btn, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(toolbar), forward_btn, FALSE, FALSE, 5);
    gtk_box_pack_end(GTK_BOX(toolbar), console_btn, FALSE, FALSE, 5);

    // WebView
    WebKitWebView* webview = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "web-context", context,
        NULL
    ));

    WebKitSettings* settings = webkit_web_view_get_settings(webview);
    webkit_settings_set_hardware_acceleration_policy(settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_allow_modal_dialogs(settings, TRUE);

    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(webview), TRUE, TRUE, 0);

    // Console panel
    console_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(console_scroll), 150);

    console_view = gtk_text_view_new();
    console_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console_view));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(console_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(console_view), TRUE);

    gtk_container_add(GTK_CONTAINER(console_scroll), console_view);
    gtk_box_pack_start(GTK_BOX(vbox), console_scroll, FALSE, FALSE, 0);
    gtk_widget_hide(console_scroll);

    // Connect signals
    g_signal_connect(back_btn, "clicked", G_CALLBACK(on_back), webview);
    g_signal_connect(forward_btn, "clicked", G_CALLBACK(on_forward), webview);
    g_signal_connect(console_btn, "clicked", G_CALLBACK(on_toggle_console), NULL);
    g_signal_connect(webview, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(webview, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(webview, "permission-request", G_CALLBACK(on_permission_request), NULL);

    // Inject console.log capture and window.open override
    WebKitUserContentManager* content_manager = webkit_web_view_get_user_content_manager(webview);
    const char* script =
        "(function() {"
        "  var oldLog = console.log;"
        "  console.log = function() {"
        "    oldLog.apply(console, arguments);"
        "    window.webkit.messageHandlers.console.postMessage(Array.from(arguments).join(' '));"
        "  };"
        "  var oldError = console.error;"
        "  console.error = function() {"
        "    oldError.apply(console, arguments);"
        "    window.webkit.messageHandlers.console.postMessage('ERROR: ' + Array.from(arguments).join(' '));"
        "  };"
        "  window.open = function(url) {"
        "    if (url) { window.location.href = url; }"
        "    return null;"
        "  };"
        "})();";

    WebKitUserScript* user_script = webkit_user_script_new(
        script,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL
    );
    webkit_user_content_manager_add_script(content_manager, user_script);

    // Register console message handler
    webkit_user_content_manager_register_script_message_handler(content_manager, "console");
    g_signal_connect(content_manager, "script-message-received::console",
        G_CALLBACK(+[](WebKitUserContentManager* manager,
                       WebKitJavascriptResult* result,
                       gpointer data) {
            JSCValue* value = webkit_javascript_result_get_js_value(result);
            char* str = jsc_value_to_string(value);

            GtkTextIter end;
            gtk_text_buffer_get_end_iter(console_buffer, &end);
            gtk_text_buffer_insert(console_buffer, &end, str, -1);
            gtk_text_buffer_insert(console_buffer, &end, "\n", -1);

            g_free(str);
        }), NULL);

    // Load URL
    std::string input = argv[1];
    if (input.find("http://") == 0 || input.find("https://") == 0) {
        webkit_web_view_load_uri(webview, input.c_str());
    } else {
        std::string uri = "file://" + input;
        webkit_web_view_load_uri(webview, uri.c_str());
    }

    gtk_widget_show_all(window);
    gtk_widget_hide(console_scroll);
    gtk_main();

    g_object_unref(context);
    g_object_unref(data_manager);

    return 0;
}
