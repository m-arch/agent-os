// agent.cpp
// Unified AI Agent - OS assistant + coding capabilities

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <termios.h>
#include <curl/curl.h>
#include <readline/readline.h>
#include <readline/history.h>

// ============== COLORS ==============
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string CYAN = "\033[36m";
const std::string RESET = "\033[0m";
const std::string BOLD = "\033[1m";

// ============== FIFO FOR GUI INPUT ==============
const char* AGENT_FIFO = "/tmp/agent-input-fifo";
int fifo_fd = -1;

void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void setup_fifo() {
    unlink(AGENT_FIFO);
    if (mkfifo(AGENT_FIFO, 0666) == -1) {
        std::cerr << YELLOW << "[Warning] Could not create FIFO" << RESET << "\n";
        return;
    }
    fifo_fd = open(AGENT_FIFO, O_RDONLY | O_NONBLOCK);
}

void cleanup_fifo() {
    if (fifo_fd >= 0) close(fifo_fd);
    unlink(AGENT_FIFO);
}

std::string check_fifo_input() {
    if (fifo_fd < 0) return "";
    char buffer[4096];
    ssize_t bytes = read(fifo_fd, buffer, sizeof(buffer) - 1);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        if (bytes > 0 && buffer[bytes-1] == '\n') buffer[bytes-1] = '\0';
        return std::string(buffer);
    }
    return "";
}

// ============== CONFIGURATION ==============
const std::string LLM_URL = "http://localhost:9090/v1/chat/completions";
const int MAX_TOKENS = 4096;
const float TEMPERATURE = 0.5;

// Global system prompt (declared here, defined later)
extern const char* SYSTEM_PROMPT;

// ============== WORKSPACE ==============
const std::string DEFAULT_WORKSPACE = "/root/workspace";
std::string active_project_dir = "";

std::string get_effective_workspace() {
    return active_project_dir.empty() ? DEFAULT_WORKSPACE : active_project_dir;
}

bool is_in_workspace(const std::string& path) {
    std::string workspace = get_effective_workspace();
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) != nullptr) {
        return std::string(resolved).find(workspace) == 0;
    }
    return path.find(workspace) == 0;
}

// ============== CURL ==============
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

// Message structure for chat API
struct Message {
    std::string role;
    std::string content;
};

// Global conversation history for chat API
std::vector<Message> conversation_history;

std::string query_llm_chat(const std::string& user_message) {
    CURL* curl = curl_easy_init();
    std::string response;

    if (curl) {
        // Build messages array JSON
        std::string messages_json = "[";

        // Add system message
        messages_json += "{\"role\":\"system\",\"content\":\"" + escape_json(SYSTEM_PROMPT) + "\"},";

        // Add conversation history
        for (const auto& msg : conversation_history) {
            messages_json += "{\"role\":\"" + msg.role + "\",\"content\":\"" + escape_json(msg.content) + "\"},";
        }

        // Add current user message
        messages_json += "{\"role\":\"user\",\"content\":\"" + escape_json(user_message) + "\"}";
        messages_json += "]";

        std::string json = "{\"messages\":" + messages_json +
                          ",\"max_tokens\":" + std::to_string(MAX_TOKENS) +
                          ",\"temperature\":" + std::to_string(TEMPERATURE) + "}";

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
            std::cerr << RED << "[Error] curl failed: " << curl_easy_strerror(res) << RESET << "\n";
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    // Parse response - look for choices[0].message.content
    size_t start = response.find("\"content\":\"");
    if (start == std::string::npos) {
        std::cerr << RED << "[Error] No content in response: " << response.substr(0, 200) << RESET << "\n";
        return "";
    }
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

    // Add to conversation history
    conversation_history.push_back({"user", user_message});
    conversation_history.push_back({"assistant", content});

    // Trim history if too long (keep last 20 messages)
    while (conversation_history.size() > 20) {
        conversation_history.erase(conversation_history.begin());
    }

    return content;
}

// Legacy function for compatibility
std::string query_llm(const std::string& prompt) {
    return query_llm_chat(prompt);
}

// ============== FILE OPERATIONS ==============
std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "[Error] Cannot read file: " + path;
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    const size_t MAX_FILE_SIZE = 8000;
    if (content.size() > MAX_FILE_SIZE) {
        content = content.substr(0, MAX_FILE_SIZE) + "\n[... truncated ...]";
    }
    return content;
}

bool create_parent_dirs(const std::string& path) {
    size_t last_slash = path.rfind('/');
    if (last_slash == std::string::npos) return true;
    std::string parent = path.substr(0, last_slash);
    if (parent.empty()) return true;
    return system(("mkdir -p \"" + parent + "\"").c_str()) == 0;
}

bool write_file(const std::string& path, const std::string& content) {
    create_parent_dirs(path);
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
        std::cerr << RED << "[Error] Text not found in file" << RESET << "\n";
        return false;
    }
    content.replace(pos, old_text.length(), new_text);
    return write_file(path, content);
}

static std::map<std::string, int> failed_commands;

std::string run_command(const std::string& cmd) {
    if (failed_commands[cmd] >= 2) {
        return "[Error: Command failed multiple times, skipping]";
    }

    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return "[Error] Failed to run command";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    int status = pclose(pipe);

    if (status != 0 || result.find("No such file") != std::string::npos) {
        failed_commands[cmd]++;
    }

    if (result.size() > 8000) {
        result = result.substr(0, 8000) + "\n[Output truncated...]";
    }
    return result;
}

std::string list_directory(const std::string& path) {
    return run_command("ls -la \"" + path + "\"");
}

// ============== DELETE WITH CONFIRMATION ==============
bool confirm_delete(const std::string& path) {
    std::string cmd = "zenity --question --title='Confirm Delete' "
                      "--text='Delete:\\n" + path + "' "
                      "--ok-label='Delete' --cancel-label='Cancel' --width=400 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

bool delete_path(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (!confirm_delete(path)) return false;
    if (S_ISDIR(st.st_mode)) {
        return system(("rm -rf \"" + path + "\"").c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

// ============== GUI ==============
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

void open_url(const std::string& url) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("agent-view", "agent-view", url.c_str(), nullptr);
        exit(1);
    }
}

// ============== DIFF DISPLAY ==============
struct Change {
    std::string file;
    std::string old_text;
    std::string new_text;
    std::string description;
};

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream stream(s);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

void show_diff(const Change& change) {
    std::cout << "\n" << BOLD << CYAN << "═══ " << change.file << " ═══" << RESET << "\n";
    if (!change.description.empty()) {
        std::cout << YELLOW << change.description << RESET << "\n";
    }

    auto old_lines = split_lines(change.old_text);
    auto new_lines = split_lines(change.new_text);

    if (!old_lines.empty()) {
        std::cout << RED << "─── Remove ───" << RESET << "\n";
        for (const auto& line : old_lines) std::cout << RED << "- " << line << RESET << "\n";
    }
    std::cout << GREEN << "─── Add ───" << RESET << "\n";
    for (const auto& line : new_lines) std::cout << GREEN << "+ " << line << RESET << "\n";
}

// ============== TAG PARSING ==============
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
    size_t start = text.find("<" + tag);
    if (start == std::string::npos) return "";
    size_t tag_end = text.find(">", start);
    std::string tag_content = text.substr(start, tag_end - start);
    size_t attr_start = tag_content.find(attr + "=\"");
    if (attr_start == std::string::npos) return "";
    attr_start += attr.length() + 2;
    size_t attr_end = tag_content.find("\"", attr_start);
    return tag_content.substr(attr_start, attr_end - attr_start);
}

std::vector<Change> parse_changes(const std::string& response) {
    std::vector<Change> changes;
    size_t pos = 0;
    while ((pos = response.find("<change", pos)) != std::string::npos) {
        Change c;
        size_t file_start = response.find("file=\"", pos) + 6;
        size_t file_end = response.find("\"", file_start);
        c.file = response.substr(file_start, file_end - file_start);

        size_t desc_start = response.find("<description>", pos);
        size_t desc_end = response.find("</description>", pos);
        if (desc_start != std::string::npos && desc_end != std::string::npos) {
            c.description = response.substr(desc_start + 13, desc_end - desc_start - 13);
        }

        size_t old_start = response.find("<old>", pos);
        size_t old_end = response.find("</old>", pos);
        if (old_start != std::string::npos && old_end != std::string::npos) {
            c.old_text = response.substr(old_start + 5, old_end - old_start - 5);
        }

        size_t new_start = response.find("<new>", pos);
        size_t new_end = response.find("</new>", pos);
        if (new_start != std::string::npos && new_end != std::string::npos) {
            c.new_text = response.substr(new_start + 5, new_end - new_start - 5);
        }

        while (!c.old_text.empty() && c.old_text[0] == '\n') c.old_text.erase(0, 1);
        while (!c.new_text.empty() && c.new_text[0] == '\n') c.new_text.erase(0, 1);
        while (!c.old_text.empty() && c.old_text.back() == '\n') c.old_text.pop_back();
        while (!c.new_text.empty() && c.new_text.back() == '\n') c.new_text.pop_back();

        if (!c.file.empty()) changes.push_back(c);

        pos = response.find("</change>", pos);
        if (pos == std::string::npos) break;
        pos++;
    }
    return changes;
}

bool apply_change(const Change& change) {
    std::string content = read_file(change.file);
    if (change.old_text.empty()) {
        return write_file(change.file, change.new_text);
    }
    if (content.find("[Error]") == 0) {
        std::cerr << RED << "Cannot read: " << change.file << RESET << "\n";
        return false;
    }
    size_t pos = content.find(change.old_text);
    if (pos == std::string::npos) {
        std::cerr << RED << "Text not found in file" << RESET << "\n";
        return false;
    }
    content.replace(pos, change.old_text.length(), change.new_text);
    return write_file(change.file, content);
}

// ============== PROJECT CONTEXT ==============
void check_project_context(const std::string& input) {
    size_t pos = input.find("[PROJECT:");
    if (pos != std::string::npos) {
        size_t start = pos + 9;
        size_t end = input.find(']', start);
        if (end != std::string::npos) {
            active_project_dir = input.substr(start, end - start);
            while (!active_project_dir.empty() && active_project_dir[0] == ' ')
                active_project_dir.erase(0, 1);
            while (!active_project_dir.empty() && active_project_dir.back() == ' ')
                active_project_dir.pop_back();
        }
    }
}

// ============== TOOL PROCESSING ==============
std::string process_tools(const std::string& response, std::string& context) {
    std::string result;

    // Process <read path="..."/>
    if (response.find("<read") != std::string::npos) {
        std::string path = extract_attribute(response, "read", "path");
        if (!path.empty()) {
            std::cout << CYAN << "[Reading: " << path << "]" << RESET << "\n";
            std::string content = read_file(path);
            result += "[Read " + path + "]\n";
            context += "\n--- " + path + " ---\n" + content + "\n";
            return result + content + "\n";
        }
    }

    // Process <list>path</list>
    size_t pos = 0;
    while ((pos = response.find("<list>", pos)) != std::string::npos) {
        size_t end = response.find("</list>", pos);
        if (end == std::string::npos) break;
        std::string path = response.substr(pos + 6, end - pos - 6);
        std::cout << CYAN << "[Listing: " << path << "]" << RESET << "\n";
        std::string listing = list_directory(path);
        std::cout << listing;
        result += "Directory " + path + ":\n" + listing + "\n";
        pos = end + 7;
    }

    // Process <read>path</read>
    pos = 0;
    while ((pos = response.find("<read>", pos)) != std::string::npos) {
        size_t end = response.find("</read>", pos);
        if (end == std::string::npos) break;
        std::string path = response.substr(pos + 6, end - pos - 6);
        std::cout << CYAN << "[Reading: " << path << "]" << RESET << "\n";
        std::string content = read_file(path);
        if (content.find("[Error]") != 0) {
            context += "\n--- " + path + " ---\n" + content + "\n";
            std::cout << GREEN << "Loaded: " << path << RESET << "\n";
        }
        result += "File " + path + " loaded\n";
        pos = end + 7;
    }

    // Process <run>cmd</run>
    pos = 0;
    while ((pos = response.find("<run>", pos)) != std::string::npos) {
        size_t end = response.find("</run>", pos);
        if (end == std::string::npos) break;
        std::string cmd = response.substr(pos + 5, end - pos - 5);
        std::cout << CYAN << "[Running: " << cmd << "]" << RESET << "\n";
        std::string output = run_command(cmd);
        std::cout << output;
        result += "$ " + cmd + "\n" + output + "\n";
        pos = end + 6;
    }

    // Process <create path="...">content</create>
    if (response.find("<create") != std::string::npos) {
        std::string path = extract_attribute(response, "create", "path");
        std::string content = extract_tag_content(response, "create");
        if (!path.empty()) {
            if (write_file(path, content)) {
                std::cout << GREEN << "[Created " << path << "]" << RESET << "\n";
                result += "[Created " + path + "]\n";
            } else {
                result += "[Error creating " + path + "]\n";
            }
        }
    }

    // Process <edit path="...">
    if (response.find("<edit") != std::string::npos) {
        std::string path = extract_attribute(response, "edit", "path");
        std::string edit_block = extract_tag_content(response, "edit");
        std::string old_text = extract_tag_content(edit_block, "old");
        std::string new_text = extract_tag_content(edit_block, "new");
        if (!path.empty() && !old_text.empty()) {
            if (edit_file(path, old_text, new_text)) {
                std::cout << GREEN << "[Edited " << path << "]" << RESET << "\n";
                result += "[Edited " + path + "]\n";
            } else {
                result += "[Error editing " + path + "]\n";
            }
        }
    }

    // Process <gui>html</gui>
    if (response.find("<gui>") != std::string::npos) {
        std::string html = extract_tag_content(response, "gui");
        if (!html.empty()) show_gui(html);
    }

    // Process <url>...</url>
    if (response.find("<url>") != std::string::npos) {
        std::string url = extract_tag_content(response, "url");
        if (!url.empty()) {
            open_url(url);
            result += "[Opening " + url + "]\n";
        }
    }

    // Process <delete path="..."/>
    if (response.find("<delete") != std::string::npos) {
        std::string path = extract_attribute(response, "delete", "path");
        if (!path.empty()) {
            if (delete_path(path)) {
                result += "[Deleted " + path + "]\n";
            } else {
                result += "[Delete cancelled/failed: " + path + "]\n";
            }
        }
    }

    // Process <change> blocks
    std::vector<Change> changes = parse_changes(response);
    for (const auto& change : changes) {
        show_diff(change);
        if (apply_change(change)) {
            std::cout << GREEN << "Applied" << RESET << "\n";
            result += "[Applied change to " + change.file + "]\n";
        } else {
            std::cout << RED << "Failed to apply" << RESET << "\n";
        }
    }

    return result;
}

// ============== SYSTEM PROMPT ==============
const char* SYSTEM_PROMPT = R"(You are Agent OS, an AI assistant for operating system tasks and coding.

IMPORTANT: Output XML tags EXACTLY as shown. Do NOT use function calling syntax.

## Tools - Use these XML tags in your response:

<list>/path/to/dir</list>      - List directory contents
<read path="/path/to/file"/>   - Read file contents
<run>shell command</run>       - Execute shell command
<create path="/path">content</create>  - Create new file
<edit path="/path"><old>text</old><new>text</new></edit>  - Edit file
<delete path="/path"/>         - Delete (with confirmation)
<gui>html content</gui>        - Show HTML interface
<url>https://...</url>         - Open URL in browser

## Code Changes (for multi-line edits):
<change file="/path">
<description>what this does</description>
<old>exact text to replace</old>
<new>replacement text</new>
</change>

## Examples:

User: list files in /root
Assistant: <list>/root</list>

User: read my bashrc
Assistant: <read path="/root/.bashrc"/>

User: run ls -la
Assistant: <run>ls -la</run>

User: create a hello world script
Assistant: <create path="/root/hello.sh">#!/bin/bash
echo "Hello World"
</create>

## Rules
- ALWAYS list/read before editing - never guess file contents
- Use full absolute paths
- Be concise - execute tools, don't over-explain
- For Google: <url>https://www.google.com/search?q=terms</url>

## Voice Input
- Commands come via speech-to-text, expect typos
- "forget context", "reset", "clear" = start fresh

## Screenshot Context
- [SCREENSHOT CONTEXT] blocks contain VL model descriptions of screenshots
- Use this context to understand what the user sees
- Make code changes based on visible code/errors in the description
)";

// ============== MAIN ==============
int main() {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    curl_global_init(CURL_GLOBAL_ALL);
    setup_fifo();

    std::string context;  // File context for @file references

    std::cout << BOLD << CYAN;
    std::cout << "═══════════════════════════════════════\n";
    std::cout << "  Agent OS - AI Assistant\n";
    std::cout << "═══════════════════════════════════════\n";
    std::cout << RESET;
    std::cout << "Commands: 'exit', 'clear', 'project <path>'\n";
    std::cout << "(Accepts voice input via widget)\n\n";

    bool is_tty = isatty(STDIN_FILENO);

    while (true) {
        std::string input;

        std::string fifo_input = check_fifo_input();
        if (!fifo_input.empty()) {
            input = fifo_input;
            std::cout << CYAN << "[Input] " << RESET << input << "\n";
        } else if (is_tty) {
            char* line = readline((BOLD + GREEN + "> " + RESET).c_str());
            if (!line) break;
            input = std::string(line);
            if (!input.empty()) add_history(line);
            free(line);
        } else {
            if (!std::getline(std::cin, input)) break;
        }

        if (input.empty()) continue;
        if (input == "exit" || input == "quit") break;

        // Check for project context
        check_project_context(input);
        if (!active_project_dir.empty()) {
            std::cout << CYAN << "[Project: " << active_project_dir << "]" << RESET << "\n";
        }

        // Extract user message (skip PROJECT context prefix)
        std::string user_message = input;
        size_t proj_end = input.find("]");
        if (proj_end != std::string::npos && input.find("[PROJECT:") != std::string::npos) {
            user_message = input.substr(proj_end + 1);
        }
        while (!user_message.empty() && (user_message[0] == ' ' || user_message[0] == '\n'))
            user_message.erase(0, 1);

        // Check for clear/reset commands
        std::string msg_lower = user_message;
        for (auto& c : msg_lower) c = tolower(c);

        bool should_clear = (msg_lower == "clear" || msg_lower == "reset" || msg_lower == "forget" ||
                            msg_lower.find("forget context") != std::string::npos ||
                            msg_lower.find("clear context") != std::string::npos ||
                            msg_lower.find("reset context") != std::string::npos ||
                            msg_lower.find("forget everything") != std::string::npos);

        if (should_clear) {
            conversation_history.clear();
            context.clear();
            active_project_dir = "";
            failed_commands.clear();
            system("rm -f /root/agent-logs/*.json");
            std::cout << GREEN << "[Context cleared]" << RESET << "\n";
            continue;
        }

        // Check for project command
        if (msg_lower.substr(0, 7) == "project" || msg_lower.substr(0, 8) == "/project") {
            std::string path = user_message.substr(user_message.find(' ') + 1);
            while (!path.empty() && path[0] == ' ') path.erase(0, 1);
            if (!path.empty() && path != user_message) {
                active_project_dir = path;
                std::cout << GREEN << "Project set: " << active_project_dir << RESET << "\n";
            } else if (active_project_dir.empty()) {
                std::cout << YELLOW << "No project set. Say 'project /path/to/dir'" << RESET << "\n";
            } else {
                std::cout << GREEN << "Current project: " << active_project_dir << RESET << "\n";
            }
            continue;
        }

        // Build user message with context
        std::string user_msg = input;
        if (!context.empty()) {
            user_msg = "Files in context:\n" + context + "\n\nUser request: " + input;
        }
        if (!active_project_dir.empty()) {
            user_msg = "[Working in project: " + active_project_dir + "]\n" + user_msg;
        }

        std::cout << BLUE << "[Thinking...]" << RESET << "\n";
        std::string response = query_llm_chat(user_msg);

        // Process tools
        std::string tool_output = process_tools(response, context);

        // Multi-turn loop if tools were used
        if (!tool_output.empty()) {
            for (int turn = 0; turn < 10; turn++) {
                // Send tool results as a follow-up message
                std::string tool_msg = "Tool Results:\n" + tool_output + "\n\nContinue processing or provide final response.";

                std::cout << BLUE << "[Thinking... turn " << (turn + 2) << "]" << RESET << "\n";
                response = query_llm_chat(tool_msg);

                tool_output = process_tools(response, context);

                if (tool_output.empty()) {
                    // No more tools - print final response
                    std::string display = response;
                    // Remove tool tags for display
                    for (const auto& tag : {"<read", "<list>", "<run>", "<gui>", "<url>", "<create", "<edit", "<delete", "<change"}) {
                        size_t p;
                        while ((p = display.find(tag)) != std::string::npos) {
                            size_t e = display.find(">", p);
                            if (display[e-1] == '/') display.erase(p, e - p + 1);
                            else {
                                std::string close = "</" + std::string(tag).substr(1) + ">";
                                size_t ce = display.find(close, p);
                                if (ce != std::string::npos) display.erase(p, ce - p + close.length());
                                else display.erase(p, e - p + 1);
                            }
                        }
                    }
                    while (!display.empty() && (display[0] == ' ' || display[0] == '\n')) display.erase(0, 1);
                    while (!display.empty() && (display.back() == ' ' || display.back() == '\n')) display.pop_back();
                    if (!display.empty()) std::cout << display << "\n";
                    break;
                }
            }
        } else {
            // No tools - just display response
            std::cout << response << "\n";
        }

        // Trim context if too large
        if (context.length() > 8000) context = context.substr(context.length() - 6000);
    }

    cleanup_fifo();
    curl_global_cleanup();
    std::cout << "Goodbye!\n";
    return 0;
}
