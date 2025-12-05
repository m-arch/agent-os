// agent.cpp
// AI Agent with file tools and GUI spawning

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <regex>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <curl/curl.h>
#include <readline/readline.h>
#include <readline/history.h>

// FIFO for GUI input
const char* AGENT_FIFO = "/tmp/agent-input-fifo";
int fifo_fd = -1;

// Signal handler to reap zombie child processes
void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// Setup FIFO for GUI input
void setup_fifo() {
    // Remove existing FIFO if present
    unlink(AGENT_FIFO);

    // Create new FIFO
    if (mkfifo(AGENT_FIFO, 0666) == -1) {
        std::cerr << "[Warning] Could not create FIFO for GUI input\n";
        return;
    }

    // Open for reading (non-blocking)
    fifo_fd = open(AGENT_FIFO, O_RDONLY | O_NONBLOCK);
    if (fifo_fd == -1) {
        std::cerr << "[Warning] Could not open FIFO\n";
    }
}

// Cleanup FIFO
void cleanup_fifo() {
    if (fifo_fd >= 0) {
        close(fifo_fd);
    }
    unlink(AGENT_FIFO);
}

// Check for input from FIFO (non-blocking)
std::string check_fifo_input() {
    if (fifo_fd < 0) return "";

    char buffer[4096];
    ssize_t bytes = read(fifo_fd, buffer, sizeof(buffer) - 1);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        // Remove trailing newline
        if (bytes > 0 && buffer[bytes-1] == '\n') {
            buffer[bytes-1] = '\0';
        }
        return std::string(buffer);
    }
    return "";
}

// ============== CONFIGURATION ==============
const std::string LLM_URL = "http://localhost:9090/completion";
const int MAX_TOKENS = 4096;
const float TEMPERATURE = 0.7;

// ============== CURL HELPERS ==============
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string escape_json(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

std::string query_llm(const std::string& prompt) {
    CURL* curl = curl_easy_init();
    std::string response;

    if (curl) {
        std::string json = "{\"prompt\": \"" + escape_json(prompt) + 
                          "\", \"n_predict\": " + std::to_string(MAX_TOKENS) + 
                          ", \"temperature\": " + std::to_string(TEMPERATURE) + 
                          ", \"stop\": [\"</s>\", \"User:\", \"<|im_end|>\", \"<|endoftext|>\"]}";

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, LLM_URL.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "[Error] curl failed: " << curl_easy_strerror(res) << "\n";
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    // Extract content from JSON response
    size_t start = response.find("\"content\":\"");
    if (start == std::string::npos) return response;
    start += 11;

    std::string content;
    bool escape = false;
    for (size_t i = start; i < response.size(); i++) {
        if (escape) {
            switch (response[i]) {
                case 'n': content += '\n'; break;
                case 't': content += '\t'; break;
                case 'r': content += '\r'; break;
                default: content += response[i];
            }
            escape = false;
        } else if (response[i] == '\\') {
            escape = true;
        } else if (response[i] == '"') {
            break;
        } else {
            content += response[i];
        }
    }
    return content;
}

// ============== FILE TOOLS ==============
std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "[Error] Cannot read file: " + path;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << content;
    return true;
}

bool edit_file(const std::string& path, const std::string& old_text, const std::string& new_text) {
    std::string content = read_file(path);
    if (content.find("[Error]") == 0) return false;

    size_t pos = content.find(old_text);
    if (pos == std::string::npos) {
        std::cerr << "[Error] Text not found in file\n";
        return false;
    }
    content.replace(pos, old_text.length(), new_text);
    return write_file(path, content);
}

std::string run_command(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "[Error] Failed to run command";

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

// ============== DELETE WITH CONFIRMATION ==============
bool confirm_delete(const std::string& path) {
    // Use zenity for GUI confirmation dialog
    std::string cmd = "zenity --question --title='Confirm Delete' "
                      "--text='Are you sure you want to delete:\\n\\n" + path + "' "
                      "--ok-label='Delete' --cancel-label='Cancel' "
                      "--width=400 2>/dev/null";
    int result = system(cmd.c_str());
    return (result == 0);  // zenity returns 0 for OK/Yes
}

bool delete_path(const std::string& path) {
    // Check if path exists
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;  // Path doesn't exist
    }

    // Show confirmation dialog
    if (!confirm_delete(path)) {
        return false;  // User cancelled
    }

    // Delete based on type
    if (S_ISDIR(st.st_mode)) {
        // Directory - use rm -rf
        std::string cmd = "rm -rf \"" + path + "\"";
        return system(cmd.c_str()) == 0;
    } else {
        // File - use unlink
        return unlink(path.c_str()) == 0;
    }
}

// ============== GUI SPAWNING ==============
static int window_counter = 0;

void show_gui(const std::string& html) {
    std::string filename = "/tmp/agent-ui-" + std::to_string(window_counter++) + ".html";
    write_file(filename, html);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("agent-view", "agent-view", filename.c_str(), nullptr);
        exit(1);
    }
}

// ============== TOOL PARSER ==============
std::string extract_tag_content(const std::string& text, const std::string& tag) {
    std::string open_tag = "<" + tag;
    std::string close_tag = "</" + tag + ">";

    size_t start = text.find(open_tag);
    if (start == std::string::npos) return "";

    size_t content_start = text.find(">", start) + 1;
    size_t end = text.find(close_tag, content_start);
    if (end == std::string::npos) return "";

    return text.substr(content_start, end - content_start);
}

std::string extract_attribute(const std::string& text, const std::string& tag, const std::string& attr) {
    std::string open_tag = "<" + tag;
    size_t start = text.find(open_tag);
    if (start == std::string::npos) return "";

    size_t tag_end = text.find(">", start);
    std::string tag_content = text.substr(start, tag_end - start);

    std::string attr_search = attr + "=\"";
    size_t attr_start = tag_content.find(attr_search);
    if (attr_start == std::string::npos) return "";

    attr_start += attr_search.length();
    size_t attr_end = tag_content.find("\"", attr_start);
    return tag_content.substr(attr_start, attr_end - attr_start);
}

std::string process_tools(const std::string& response) {
    std::string result;
    std::string remaining = response;

    // Process <read path="..."/>
    if (remaining.find("<read") != std::string::npos) {
        std::string path = extract_attribute(remaining, "read", "path");
        if (!path.empty()) {
            std::string content = read_file(path);
            result += "[Read " + path + "]\n";
            return result + "\n---FILE CONTENT---\n" + content + "\n---END---\n";
        }
    }

    // Process <create path="...">content</create>
    if (remaining.find("<create") != std::string::npos) {
        std::string path = extract_attribute(remaining, "create", "path");
        std::string content = extract_tag_content(remaining, "create");
        if (!path.empty()) {
            if (write_file(path, content)) {
                result += "[Created " + path + "]\n";
            } else {
                result += "[Error creating " + path + "]\n";
            }
        }
    }

    // Process <edit path="..."><old>...</old><new>...</new></edit>
    if (remaining.find("<edit") != std::string::npos) {
        std::string path = extract_attribute(remaining, "edit", "path");
        std::string edit_block = extract_tag_content(remaining, "edit");
        std::string old_text = extract_tag_content(edit_block, "old");
        std::string new_text = extract_tag_content(edit_block, "new");

        if (!path.empty() && !old_text.empty()) {
            if (edit_file(path, old_text, new_text)) {
                result += "[Edited " + path + "]\n";
            } else {
                result += "[Error editing " + path + "]\n";
            }
        }
    }

    // Process <run>command</run>
    if (remaining.find("<run>") != std::string::npos) {
        std::string cmd = extract_tag_content(remaining, "run");
        if (!cmd.empty()) {
            result += "[Running: " + cmd + "]\n";
            result += run_command(cmd);
        }
    }

    // Process <gui>html</gui>
    if (remaining.find("<gui>") != std::string::npos) {
        std::string html = extract_tag_content(remaining, "gui");
        if (!html.empty()) {
            show_gui(html);
        }
    }


    // Process url
    if (remaining.find("<url>") != std::string::npos) {
      std::string url = extract_tag_content(remaining, "url");
      if (!url.empty()) {
        pid_t pid = fork();
        if (pid == 0) {
	  setsid();
	  execlp("agent-view", "agent-view", url.c_str(), nullptr);
	  exit(1);
        }
        result += "[Opening " + url + "]\n";
      }
    }

    // Process <delete path="..."/> - requires GUI confirmation
    if (remaining.find("<delete") != std::string::npos) {
        std::string path = extract_attribute(remaining, "delete", "path");
        if (!path.empty()) {
            result += "[Delete requested: " + path + "]\n";
            if (delete_path(path)) {
                result += "[Deleted " + path + "]\n";
            } else {
                result += "[Delete cancelled or failed for " + path + "]\n";
            }
        }
    }

    return result;
}

// ============== MAIN ==============
const char* SYSTEM_PROMPT = R"(You are Agent OS, an AI operating system assistant. You help users by reading, editing, and creating files, running commands, and generating graphical interfaces.

## Available Tools

1. READ FILE:
<read path="/path/to/file"/>

2. CREATE FILE:
<create path="/path/to/file">
file content here
</create>

3. EDIT FILE (replace text):
<edit path="/path/to/file">
<old>exact text to find</old>
<new>replacement text</new>
</edit>

4. RUN COMMAND:
<run>command here</run>

5. SHOW GUI (HTML interface):
<gui>
<!DOCTYPE html>
<html>
<head><title>App</title></head>
<body>
your app here
</body>
</html>
</gui>

6. OPEN URL (for web browsing, searching):
<url>https://example.com</url>

7. DELETE FILE/FOLDER (shows confirmation dialog):
<delete path="/path/to/file_or_folder"/>

## Guidelines
- When asked to open a website or search something online, use <url> with the appropriate URL
- For Google searches, use: <url>https://www.google.com/search?q=your+search+terms</url>
- When asked to view/read a file, use <read>
- When asked to modify a file, first <read> it, then <edit> with exact matching text
- When asked to create an app/interface, use <gui> with complete HTML
- For terminal/shell commands, use <run>
- When asked to delete/remove a file or folder, use <delete> - a confirmation dialog will appear
- Be concise. Execute tools without excessive explanation.
- Do NOT use code-agent unless explicitly asked for code review.
)";

int main() {
    // Set up SIGCHLD handler to reap zombie processes
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    curl_global_init(CURL_GLOBAL_ALL);

    // Setup FIFO for GUI input
    setup_fifo();

    std::string history;

    std::cout << "═══════════════════════════════════════\n";
    std::cout << "  Agent OS - AI-Powered Operating System\n";
    std::cout << "═══════════════════════════════════════\n";
    std::cout << "Commands: 'exit' to quit, 'clear' to reset\n";
    std::cout << "(Also accepts input from GUI widget)\n\n";

    bool is_tty = isatty(STDIN_FILENO);

    while (true) {
        std::string input;

        // Check for GUI input first
        std::string fifo_input = check_fifo_input();
        if (!fifo_input.empty()) {
            input = fifo_input;
            std::cout << "\033[1;36m[GUI Input]\033[0m " << input << "\n";
        } else if (is_tty) {
            char* line = readline("\033[1;32m>\033[0m ");
            if (!line) break;
            input = std::string(line);
            if (!input.empty()) {
                add_history(line);
            }
            free(line);
        } else {
            if (!std::getline(std::cin, input)) break;
        }

        if (input.empty()) continue;
        if (input == "exit" || input == "quit") break;
        if (input == "clear") {
            history.clear();
            std::cout << "[Context cleared]\n";
            continue;
        }

        // Build prompt with history
        std::string prompt = std::string(SYSTEM_PROMPT) + "\n\n" + history + "User: " + input + "\nAssistant:";

        std::cout << "\033[1;34m[Thinking...]\033[0m\n";
        std::string response = query_llm(prompt);

        // Check for HTML (direct GUI output without <gui> tags)
        if ((response.find("<!DOCTYPE") != std::string::npos || 
             response.find("<html") != std::string::npos) &&
            response.find("<gui>") == std::string::npos) {
            show_gui(response);
        }

        // Process any tool calls
        std::string tool_output = process_tools(response);

        // Clean response for display (remove tool XML)
        std::string display = response;
        // Simple cleanup - remove tool tags for display
        size_t pos;
        while ((pos = display.find("<read")) != std::string::npos) {
            size_t end = display.find("/>", pos);
            if (end != std::string::npos) display.erase(pos, end - pos + 2);
        }
        while ((pos = display.find("<create")) != std::string::npos) {
            size_t end = display.find("</create>", pos);
            if (end != std::string::npos) display.erase(pos, end - pos + 9);
        }
        while ((pos = display.find("<edit")) != std::string::npos) {
            size_t end = display.find("</edit>", pos);
            if (end != std::string::npos) display.erase(pos, end - pos + 7);
        }
        while ((pos = display.find("<run>")) != std::string::npos) {
            size_t end = display.find("</run>", pos);
            if (end != std::string::npos) display.erase(pos, end - pos + 6);
        }
        while ((pos = display.find("<gui>")) != std::string::npos) {
            size_t end = display.find("</gui>", pos);
            if (end != std::string::npos) display.erase(pos, end - pos + 6);
        }
        while ((pos = display.find("<delete")) != std::string::npos) {
            size_t end = display.find("/>", pos);
            if (end != std::string::npos) display.erase(pos, end - pos + 2);
        }
        while ((pos = display.find("<url>")) != std::string::npos) {
            size_t end = display.find("</url>", pos);
            if (end != std::string::npos) display.erase(pos, end - pos + 6);
        }

        // Trim and display
        while (!display.empty() && (display[0] == ' ' || display[0] == '\n')) display.erase(0, 1);
        while (!display.empty() && (display.back() == ' ' || display.back() == '\n')) display.pop_back();

        if (!display.empty()) {
            std::cout << display << "\n";
        }
        if (!tool_output.empty()) {
            std::cout << tool_output;
        }

        // Update history
        history += "User: " + input + "\nAssistant: " + response + "\n";

        // Keep history manageable
        if (history.length() > 8000) {
            history = history.substr(history.length() - 6000);
        }
    }

    cleanup_fifo();
    curl_global_cleanup();
    std::cout << "Goodbye!\n";
    return 0;
}
