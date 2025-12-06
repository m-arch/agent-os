// code-agent.cpp
// Interactive coding agent with diff review

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <termios.h>
#include <limits.h>
#include <curl/curl.h>

// ============== CONFIGURATION ==============
const std::string LLM_URL = "http://localhost:9090/completion";
const int MAX_TOKENS = 4096;
const float TEMPERATURE = 0.3;  // Lower for more precise code

// ============== COLORS ==============
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string CYAN = "\033[36m";
const std::string RESET = "\033[0m";
const std::string BOLD = "\033[1m";

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

std::string decode_html_entities(const std::string& s) {
    std::string result = s;
    
    // Replace common HTML entities
    size_t pos;
    while ((pos = result.find("&lt;")) != std::string::npos)
        result.replace(pos, 4, "<");
    while ((pos = result.find("&gt;")) != std::string::npos)
        result.replace(pos, 4, ">");
    while ((pos = result.find("&amp;")) != std::string::npos)
        result.replace(pos, 5, "&");
    while ((pos = result.find("&quot;")) != std::string::npos)
        result.replace(pos, 6, "\"");
    while ((pos = result.find("&apos;")) != std::string::npos)
        result.replace(pos, 6, "'");
    while ((pos = result.find("&#39;")) != std::string::npos)
        result.replace(pos, 5, "'");
    while ((pos = result.find("&#x27;")) != std::string::npos)
        result.replace(pos, 6, "'");
    
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

        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    // Parse response
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
    return decode_html_entities(content);
}

// ============== WORKSPACE ==============
const std::string DEFAULT_WORKSPACE = "/root/workspace";
std::string active_project_dir = "";  // Set when [PROJECT: path] is detected

std::string get_effective_workspace() {
    // If a project directory is set, use it; otherwise use default workspace
    return active_project_dir.empty() ? DEFAULT_WORKSPACE : active_project_dir;
}

bool is_in_workspace(const std::string& path) {
    std::string workspace = get_effective_workspace();
    // Resolve to absolute path and check it's in workspace
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) != nullptr) {
        return std::string(resolved).find(workspace) == 0;
    }
    // For new files that don't exist yet, check the path directly
    return path.find(workspace) == 0;
}

// Extract project directory from input if present
void check_project_context(const std::string& input) {
    // Look for PROJECT CONTEXT block or [PROJECT: path] tag
    size_t pos = input.find("CURRENT PROJECT:");
    if (pos != std::string::npos) {
        size_t start = pos + 17;  // Length of "CURRENT PROJECT: "
        size_t end = input.find('\n', start);
        if (end != std::string::npos) {
            active_project_dir = input.substr(start, end - start);
            // Trim whitespace
            while (!active_project_dir.empty() && active_project_dir.back() == ' ')
                active_project_dir.pop_back();
            while (!active_project_dir.empty() && active_project_dir[0] == ' ')
                active_project_dir.erase(0, 1);
        }
    }
    // Also check for [PROJECT: path] format
    pos = input.find("[PROJECT:");
    if (pos != std::string::npos) {
        size_t start = pos + 9;  // Length of "[PROJECT:"
        size_t end = input.find(']', start);
        if (end != std::string::npos) {
            active_project_dir = input.substr(start, end - start);
            // Trim whitespace
            while (!active_project_dir.empty() && active_project_dir.back() == ' ')
                active_project_dir.pop_back();
            while (!active_project_dir.empty() && active_project_dir[0] == ' ')
                active_project_dir.erase(0, 1);
        }
    }
}

// ============== FILE OPS ==============
std::string read_file(const std::string& path) {
    if (!is_in_workspace(path)) {
        return "[Error: Path outside workspace]";
    }
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Limit file content to prevent context overflow
    const size_t MAX_FILE_SIZE = 4000;
    if (content.size() > MAX_FILE_SIZE) {
        content = content.substr(0, MAX_FILE_SIZE) + "\n[... truncated, " +
                  std::to_string(content.size() - MAX_FILE_SIZE) + " more bytes ...]";
    }
    return content;
}

bool create_parent_dirs(const std::string& path) {
    // Extract parent directory
    size_t last_slash = path.rfind('/');
    if (last_slash == std::string::npos) return true;

    std::string parent = path.substr(0, last_slash);
    if (parent.empty()) return true;

    std::string cmd = "mkdir -p \"" + parent + "\"";
    return system(cmd.c_str()) == 0;
}

bool write_file(const std::string& path, const std::string& content) {
    if (!is_in_workspace(path)) {
        std::cerr << RED << "Error: Cannot write outside workspace" << RESET << "\n";
        return false;
    }

    // Create parent directories if they don't exist
    if (!create_parent_dirs(path)) {
        std::cerr << RED << "Error: Cannot create parent directories" << RESET << "\n";
        return false;
    }

    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << content;
    return true;
}

std::string list_directory(const std::string& path) {
    if (!is_in_workspace(path)) {
        return "[Error: Path outside workspace]";
    }
    std::string cmd = "ls -la \"" + path + "\" 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "[Error: Cannot list directory]";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

// Track failed commands to prevent loops
static std::map<std::string, int> failed_commands;

std::string run_command(const std::string& cmd) {
    // Security: Only allow commands that operate within workspace
    // Check for dangerous patterns
    if (cmd.find("rm -rf") != std::string::npos ||
        cmd.find("sudo") != std::string::npos ||
        cmd.find("chmod") != std::string::npos ||
        cmd.find("chown") != std::string::npos ||
        cmd.find("dd ") != std::string::npos ||
        cmd.find("> /") != std::string::npos) {
        return "[Error: Command not allowed for security reasons]";
    }

    // Check if this command has failed too many times
    if (failed_commands[cmd] >= 2) {
        return "[Error: Command failed multiple times, skipping to prevent loop]";
    }

    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");  // Capture stderr too
    if (!pipe) return "[Error: Cannot execute command]";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    int status = pclose(pipe);

    // Track failures
    if (status != 0 || result.find("No such file") != std::string::npos ||
        result.find("not found") != std::string::npos ||
        result.find("Error") != std::string::npos) {
        failed_commands[cmd]++;
    }

    // Limit output size
    if (result.size() > 8000) {
        result = result.substr(0, 8000) + "\n[Output truncated...]";
    }
    return result;
}

// ============== DIFF ==============
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
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

void show_diff(const Change& change) {
    std::cout << "\n" << BOLD << CYAN << "═══ " << change.file << " ═══" << RESET << "\n";
    if (!change.description.empty()) {
        std::cout << YELLOW << change.description << RESET << "\n";
    }
    std::cout << "\n";

    auto old_lines = split_lines(change.old_text);
    auto new_lines = split_lines(change.new_text);

    std::cout << RED << "─── Remove ───" << RESET << "\n";
    for (const auto& line : old_lines) {
        std::cout << RED << "- " << line << RESET << "\n";
    }

    std::cout << GREEN << "─── Add ───" << RESET << "\n";
    for (const auto& line : new_lines) {
        std::cout << GREEN << "+ " << line << RESET << "\n";
    }
    std::cout << "\n";
}

// ============== INPUT ==============
char get_single_char() {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    char c = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return c;
}

std::string get_multiline_input() {
    std::cout << YELLOW << "(Enter your edit, Ctrl+D when done)" << RESET << "\n";
    std::string result, line;
    while (std::getline(std::cin, line)) {
        result += line + "\n";
    }
    std::cin.clear();
    clearerr(stdin);
    return result;
}

// ============== PARSE CHANGES ==============
std::vector<Change> parse_changes(const std::string& response) {
    std::vector<Change> changes;
    
    size_t pos = 0;
    while ((pos = response.find("<change", pos)) != std::string::npos) {
        Change c;
        
        // Get file attribute
        size_t file_start = response.find("file=\"", pos) + 6;
        size_t file_end = response.find("\"", file_start);
        c.file = response.substr(file_start, file_end - file_start);
        
        // Get description
        size_t desc_start = response.find("<description>", pos);
        size_t desc_end = response.find("</description>", pos);
        if (desc_start != std::string::npos && desc_end != std::string::npos) {
            desc_start += 13;
            c.description = response.substr(desc_start, desc_end - desc_start);
        }
        
        // Get old text
        size_t old_start = response.find("<old>", pos);
        size_t old_end = response.find("</old>", pos);
        if (old_start != std::string::npos && old_end != std::string::npos) {
            old_start += 5;
            c.old_text = response.substr(old_start, old_end - old_start);
        }
        
        // Get new text
        size_t new_start = response.find("<new>", pos);
        size_t new_end = response.find("</new>", pos);
        if (new_start != std::string::npos && new_end != std::string::npos) {
            new_start += 5;
            c.new_text = response.substr(new_start, new_end - new_start);
        }
        
        // Trim leading/trailing newlines
        while (!c.old_text.empty() && c.old_text[0] == '\n') c.old_text.erase(0, 1);
        while (!c.new_text.empty() && c.new_text[0] == '\n') c.new_text.erase(0, 1);
        while (!c.old_text.empty() && c.old_text.back() == '\n') c.old_text.pop_back();
        while (!c.new_text.empty() && c.new_text.back() == '\n') c.new_text.pop_back();
        
        if (!c.file.empty()) {
            changes.push_back(c);
        }
        
        pos = response.find("</change>", pos);
        if (pos == std::string::npos) break;
        pos++;
    }
    
    return changes;
}

// ============== APPLY CHANGE ==============
bool apply_change(const Change& change) {
    std::string content = read_file(change.file);
    if (content.empty() && !change.old_text.empty()) {
        std::cerr << RED << "Cannot read file: " << change.file << RESET << "\n";
        return false;
    }
    
    // New file
    if (change.old_text.empty()) {
        return write_file(change.file, change.new_text);
    }
    
    // Find and replace
    size_t pos = content.find(change.old_text);
    if (pos == std::string::npos) {
        std::cerr << RED << "Text not found in file" << RESET << "\n";
        return false;
    }
    
    content.replace(pos, change.old_text.length(), change.new_text);
    return write_file(change.file, content);
}

// ============== MAIN ==============
const char* SYSTEM_PROMPT = R"(Expert coding assistant.

TOOLS:
<list>path</list> - List dir
<read>path</read> - Read file
<run>cmd</run> - Shell command

EDIT (read first):
<change file="path">
<description>what</description>
<old>exact text</old>
<new>new text</new>
</change>

NEW FILE:
<change file="path">
<description>new</description>
<old></old>
<new>content</new>
</change>

RULES:
- List before read, read before edit. Exact text in <old>.
- IMPORTANT: If a PROJECT CONTEXT is provided, work ONLY within that project directory.
- Do NOT navigate to parent directories or list files outside the specified project.
- Stay focused on the current task and project files only.

CRITICAL - NO HALLUCINATION:
- NEVER assume or invent file names, directory structures, or file contents
- You MUST use <list>path</list> FIRST to see what actually exists
- ONLY describe files you have ACTUALLY listed or read with tools
- If you haven't run <list> or <read>, you don't know what's there
- WRONG: "The files are: main.py, utils.py" (without listing first)
- RIGHT: <list>/path/to/project</list> then describe what you see
- Do NOT assume a project is Python just because you expect it to be
- Look at ACTUAL file extensions: .js/.ts = JavaScript/TypeScript, .py = Python
- NEVER run commands like "python main.py" unless you SEE main.py in directory listing
- If a command fails, do NOT retry it - move on to something else

VOICE INPUT:
- Commands come via voice transcription, expect typos/phonetic errors
- Interpret: "forgit" = "forget", "kontekst" = "context", "fil" = "file", etc.
- "forget context", "reset", "clear" = ignore previous conversation, start fresh
- Focus on intent, not exact spelling
)";

// Analyze prompt template - %s gets replaced with project path
const char* ANALYZE_PROMPT_TEMPLATE = R"(Analyze codebase at: %s

1. <run>find %s -type f \( -name "*.cpp" -o -name "*.py" -o -name "*.js" -o -name "*.h" \) 2>/dev/null</run>
2. <list>%s</list>
3. Read each file, note imports
4. Create %s/ANALYSIS.md with: overview, files, dependencies, architecture
)";

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_ALL);
    
    std::string context;
    std::string history;
    
    std::cout << BOLD << CYAN;
    std::cout << "╔═══════════════════════════════════════╗\n";
    std::cout << "║         Code Agent                    ║\n";
    std::cout << "║   Interactive Code Review Assistant   ║\n";
    std::cout << "╚═══════════════════════════════════════╝\n";
    std::cout << RESET;
    std::cout << "Commands:\n";
    std::cout << "  " << YELLOW << "/project <path>" << RESET << " - Set/show active project directory\n";
    std::cout << "  " << YELLOW << "/analyze <path>" << RESET << " - Systematic codebase analysis\n";
    std::cout << "  " << YELLOW << "/file <path>" << RESET << "    - Load file into context\n";
    std::cout << "  " << YELLOW << "/clear" << RESET << "          - Clear context (and project)\n";
    std::cout << "  " << YELLOW << "/exit" << RESET << "           - Quit\n\n";
    
    while (true) {
        std::cout << BOLD << GREEN << ">>> " << RESET;
        std::string input;
        if (!std::getline(std::cin, input)) break;
        
        if (input.empty()) continue;

        // Check for project context in input and set active_project_dir
        check_project_context(input);
        if (!active_project_dir.empty()) {
            std::cout << CYAN << "[Project: " << active_project_dir << "]" << RESET << "\n";
        }

        // Extract actual user message (skip any injected PROJECT CONTEXT)
        std::string user_message = input;

        // If input contains PROJECT CONTEXT block, extract the part after it
        size_t ctx_end = input.find("=== END PROJECT CONTEXT ===");
        if (ctx_end != std::string::npos) {
            user_message = input.substr(ctx_end + 27);  // Skip past the marker
        }
        // Also try to find message after [PROJECT:...] tag
        size_t proj_end = input.find("]");
        if (proj_end != std::string::npos && input.find("[PROJECT:") != std::string::npos) {
            std::string after_tag = input.substr(proj_end + 1);
            // If this is shorter, it's probably the actual message
            if (after_tag.length() < user_message.length()) {
                user_message = after_tag;
            }
        }

        // Trim whitespace
        while (!user_message.empty() && (user_message[0] == ' ' || user_message[0] == '\n'))
            user_message.erase(0, 1);
        while (!user_message.empty() && (user_message.back() == ' ' || user_message.back() == '\n'))
            user_message.pop_back();

        // Convert to lowercase for comparison
        std::string msg_lower = user_message;
        for (auto& c : msg_lower) c = tolower(c);

        // Check for clear commands - be very inclusive
        bool should_clear = (msg_lower == "forget") ||
                           (msg_lower == "reset") ||
                           (msg_lower == "clear") ||
                           (msg_lower == "clear context") ||
                           (msg_lower == "forget context") ||
                           (msg_lower == "reset context") ||
                           (msg_lower == "forget everything") ||
                           (msg_lower.find("forget") == 0 && msg_lower.find("context") != std::string::npos) ||
                           (msg_lower.find("clear") == 0 && msg_lower.find("context") != std::string::npos) ||
                           (msg_lower.find("reset") == 0 && msg_lower.find("context") != std::string::npos);

        if (should_clear) {
            context.clear();
            history.clear();
            active_project_dir = "";
            // Clear log files
            system("rm -f /root/agent-logs/coding-history.json");
            system("rm -f /root/agent-logs/main-history.json");
            std::cout << GREEN << "Context and logs cleared." << RESET << "\n";
            continue;
        }

        // Commands
        if (input[0] == '/') {
            if (input == "/exit" || input == "/quit") break;

            if (input == "/clear") {
                context.clear();
                history.clear();
                active_project_dir = "";  // Also clear project context
                // Clear log files
                system("rm -f /root/agent-logs/coding-history.json");
                system("rm -f /root/agent-logs/main-history.json");
                std::cout << GREEN << "Context and logs cleared." << RESET << "\n";
                continue;
            }

            if (input.substr(0, 8) == "/project") {
                std::string path = input.substr(8);
                while (!path.empty() && path[0] == ' ') path.erase(0, 1);
                if (path.empty()) {
                    if (active_project_dir.empty()) {
                        std::cout << YELLOW << "No project set. Use: /project <path>" << RESET << "\n";
                    } else {
                        std::cout << GREEN << "Current project: " << active_project_dir << RESET << "\n";
                    }
                } else {
                    if (is_in_workspace(path) || path.find("/root/workspace") == 0) {
                        active_project_dir = path;
                        std::cout << GREEN << "Project set to: " << active_project_dir << RESET << "\n";
                    } else {
                        std::cout << RED << "Project must be in /root/workspace" << RESET << "\n";
                    }
                }
                continue;
            }
            
            if (input.substr(0, 5) == "/file") {
                std::string path = input.substr(6);
                while (!path.empty() && path[0] == ' ') path.erase(0, 1);

                std::string content = read_file(path);
                if (content.empty()) {
                    std::cout << RED << "Cannot read: " << path << RESET << "\n";
                } else {
                    context += "\n--- " + path + " ---\n" + content + "\n";
                    std::cout << GREEN << "Loaded: " << path << " (" << content.size() << " bytes)" << RESET << "\n";
                }
                continue;
            }

            if (input.substr(0, 8) == "/analyze") {
                std::string path = input.substr(8);
                while (!path.empty() && path[0] == ' ') path.erase(0, 1);

                if (path.empty()) {
                    std::cout << RED << "Usage: /analyze <path>" << RESET << "\n";
                    continue;
                }

                // Check path exists
                if (!is_in_workspace(path)) {
                    std::cout << RED << "Path must be in /root/workspace" << RESET << "\n";
                    continue;
                }

                std::cout << BOLD << CYAN << "\n═══════════════════════════════════════\n";
                std::cout << "  ANALYZING: " << path << "\n";
                std::cout << "═══════════════════════════════════════\n" << RESET;

                // Clear context for fresh analysis
                context.clear();
                history.clear();

                // Format the analyze prompt with the path
                char analyze_prompt[4096];
                snprintf(analyze_prompt, sizeof(analyze_prompt), ANALYZE_PROMPT_TEMPLATE,
                         path.c_str(), path.c_str(), path.c_str(), path.c_str());

                // Multi-turn analysis loop
                std::string analysis_history;
                int max_turns = 20;  // Limit iterations
                bool analysis_complete = false;

                for (int turn = 0; turn < max_turns && !analysis_complete; turn++) {
                    std::string prompt;
                    if (turn == 0) {
                        prompt = std::string(SYSTEM_PROMPT) + "\n\n" + analyze_prompt + "\nAssistant:";
                    } else {
                        prompt = std::string(SYSTEM_PROMPT) + "\n\n";
                        if (!context.empty()) {
                            prompt += "Files read so far:\n" + context + "\n\n";
                        }
                        prompt += analysis_history + "\nContinue the analysis. Read more files or generate the final ANALYSIS.md report.\nAssistant:";
                    }

                    std::cout << BLUE << "[Turn " << (turn + 1) << " - Analyzing...]" << RESET << "\n";
                    std::string response = query_llm(prompt);

                    // Process all tools in response
                    std::string tool_results;
                    size_t pos = 0;

                    // Process all <run> commands
                    while ((pos = response.find("<run>", pos)) != std::string::npos) {
                        size_t end = response.find("</run>", pos);
                        if (end == std::string::npos) break;
                        std::string cmd = response.substr(pos + 5, end - pos - 5);
                        std::cout << CYAN << "[Running: " << cmd << "]" << RESET << "\n";
                        std::string output = run_command(cmd);
                        std::cout << output << "\n";
                        tool_results += "Command: " + cmd + "\nOutput:\n" + output + "\n";
                        pos = end + 6;
                    }

                    // Process all <list> commands
                    pos = 0;
                    while ((pos = response.find("<list>", pos)) != std::string::npos) {
                        size_t end = response.find("</list>", pos);
                        if (end == std::string::npos) break;
                        std::string list_path = response.substr(pos + 6, end - pos - 6);
                        std::cout << CYAN << "[Listing: " << list_path << "]" << RESET << "\n";
                        std::string listing = list_directory(list_path);
                        std::cout << listing << "\n";
                        tool_results += "Directory: " + list_path + "\n" + listing + "\n";
                        pos = end + 7;
                    }

                    // Process all <read> commands
                    pos = 0;
                    while ((pos = response.find("<read>", pos)) != std::string::npos) {
                        size_t end = response.find("</read>", pos);
                        if (end == std::string::npos) break;
                        std::string read_path = response.substr(pos + 6, end - pos - 6);
                        std::cout << CYAN << "[Reading: " << read_path << "]" << RESET << "\n";
                        std::string file_content = read_file(read_path);
                        if (!file_content.empty() && file_content.find("[Error") != 0) {
                            context += "\n--- " + read_path + " ---\n" + file_content + "\n";
                            std::cout << GREEN << "Loaded: " << read_path << " (" << file_content.size() << " bytes)" << RESET << "\n";
                            tool_results += "Read file: " + read_path + "\n";
                        } else {
                            std::cout << RED << "Cannot read: " << read_path << RESET << "\n";
                        }
                        pos = end + 7;
                    }

                    // Process <change> for ANALYSIS.md creation
                    std::vector<Change> changes = parse_changes(response);
                    for (const auto& change : changes) {
                        if (change.file.find("ANALYSIS.md") != std::string::npos) {
                            std::cout << BOLD << GREEN << "\n[Creating ANALYSIS.md]" << RESET << "\n";
                            if (apply_change(change)) {
                                std::cout << GREEN << "✓ ANALYSIS.md created successfully!" << RESET << "\n";
                                analysis_complete = true;
                            } else {
                                std::cout << RED << "✗ Failed to create ANALYSIS.md" << RESET << "\n";
                            }
                        } else {
                            show_diff(change);
                            if (apply_change(change)) {
                                std::cout << GREEN << "✓ Applied" << RESET << "\n";
                            }
                        }
                    }

                    // Add to history
                    analysis_history += "Assistant: " + response + "\n";
                    if (!tool_results.empty()) {
                        analysis_history += "Tool Results:\n" + tool_results + "\n";
                    }

                    // Trim history aggressively to stay under context limit
                    if (analysis_history.length() > 3000) {
                        analysis_history = analysis_history.substr(analysis_history.length() - 2000);
                    }

                    // Check if analysis looks complete
                    if (changes.empty() && tool_results.empty()) {
                        std::cout << response << "\n";
                        // If no tools used, might need to prompt for continuation
                        if (response.find("ANALYSIS.md") != std::string::npos ||
                            response.find("complete") != std::string::npos ||
                            response.find("finished") != std::string::npos) {
                            analysis_complete = true;
                        }
                    }
                }

                if (analysis_complete) {
                    std::cout << BOLD << GREEN << "\n═══════════════════════════════════════\n";
                    std::cout << "  ANALYSIS COMPLETE\n";
                    std::cout << "═══════════════════════════════════════\n" << RESET;
                } else {
                    std::cout << YELLOW << "\nAnalysis reached turn limit. Check results.\n" << RESET;
                }
                continue;
            }

            std::cout << RED << "Unknown command" << RESET << "\n";
            continue;
        }
        
        // Build prompt
        std::string prompt = std::string(SYSTEM_PROMPT) + "\n\n";
        if (!context.empty()) {
            prompt += "Current files:\n" + context + "\n\n";
        }
        prompt += history + "User: " + input + "\nAssistant:";
        
        std::cout << BLUE << "[Thinking...]" << RESET << "\n";
        std::string response = query_llm(prompt);

        // Process tools in response
        std::string tool_output;
        size_t pos;

        // Process all <run> commands
        pos = 0;
        while ((pos = response.find("<run>", pos)) != std::string::npos) {
            size_t end = response.find("</run>", pos);
            if (end == std::string::npos) break;
            std::string cmd = response.substr(pos + 5, end - pos - 5);
            std::cout << CYAN << "[Running: " << cmd << "]" << RESET << "\n";
            std::string output = run_command(cmd);
            std::cout << output << "\n";
            tool_output += "Command: " + cmd + "\nOutput:\n" + output + "\n";
            pos = end + 6;
        }

        // Process all <list> commands
        pos = 0;
        while ((pos = response.find("<list>", pos)) != std::string::npos) {
            size_t end = response.find("</list>", pos);
            if (end == std::string::npos) break;
            std::string list_path = response.substr(pos + 6, end - pos - 6);
            std::cout << CYAN << "[Listing: " << list_path << "]" << RESET << "\n";
            std::string listing = list_directory(list_path);
            std::cout << listing << "\n";
            tool_output += "Directory listing of " + list_path + ":\n" + listing + "\n";
            pos = end + 7;
        }

        // Process all <read> commands
        pos = 0;
        while ((pos = response.find("<read>", pos)) != std::string::npos) {
            size_t end = response.find("</read>", pos);
            if (end == std::string::npos) break;
            std::string read_path = response.substr(pos + 6, end - pos - 6);
            std::cout << CYAN << "[Reading: " << read_path << "]" << RESET << "\n";
            std::string file_content = read_file(read_path);
            if (!file_content.empty() && file_content.find("[Error") != 0) {
                context += "\n--- " + read_path + " ---\n" + file_content + "\n";
                std::cout << GREEN << "Loaded: " << read_path << " (" << file_content.size() << " bytes)" << RESET << "\n";
                tool_output += "File content added to context: " + read_path + "\n";
            } else {
                std::cout << RED << "Cannot read: " << read_path << RESET << "\n";
            }
            pos = end + 7;
        }

        // Parse changes
        std::vector<Change> changes = parse_changes(response);

        // Apply any changes
        if (!changes.empty()) {
            std::cout << "\n" << BOLD << "Found " << changes.size() << " change(s)" << RESET << "\n";
            for (size_t i = 0; i < changes.size(); i++) {
                Change& change = changes[i];
                std::cout << BOLD << "\n[" << (i+1) << "/" << changes.size() << "]" << RESET;
                show_diff(change);
                if (apply_change(change)) {
                    std::cout << GREEN << "✓ Applied" << RESET << "\n";
                } else {
                    std::cout << RED << "✗ Failed to apply" << RESET << "\n";
                }
            }
        }

        // If tools were used, continue with multi-turn loop to let LLM process results
        if (!tool_output.empty()) {
            std::string turn_history = "User: " + input + "\nAssistant: " + response + "\nTool Results:\n" + tool_output;
            int max_turns = 10;

            for (int turn = 0; turn < max_turns; turn++) {
                // Build continuation prompt
                std::string cont_prompt = std::string(SYSTEM_PROMPT) + "\n\n";
                if (!context.empty()) {
                    cont_prompt += "Current files:\n" + context + "\n\n";
                }
                cont_prompt += turn_history + "\nContinue exploring or create the requested output.\nAssistant:";

                std::cout << BLUE << "[Thinking... turn " << (turn + 2) << "]" << RESET << "\n";
                response = query_llm(cont_prompt);

                // Process tools in this response
                tool_output.clear();
                size_t pos;

                // Process <list> commands
                pos = 0;
                while ((pos = response.find("<list>", pos)) != std::string::npos) {
                    size_t end = response.find("</list>", pos);
                    if (end == std::string::npos) break;
                    std::string list_path = response.substr(pos + 6, end - pos - 6);
                    std::cout << CYAN << "[Listing: " << list_path << "]" << RESET << "\n";
                    std::string listing = list_directory(list_path);
                    std::cout << listing << "\n";
                    tool_output += "Directory " + list_path + ":\n" + listing + "\n";
                    pos = end + 7;
                }

                // Process <read> commands
                pos = 0;
                while ((pos = response.find("<read>", pos)) != std::string::npos) {
                    size_t end = response.find("</read>", pos);
                    if (end == std::string::npos) break;
                    std::string read_path = response.substr(pos + 6, end - pos - 6);
                    std::cout << CYAN << "[Reading: " << read_path << "]" << RESET << "\n";
                    std::string file_content = read_file(read_path);
                    if (!file_content.empty() && file_content.find("[Error") != 0) {
                        context += "\n--- " + read_path + " ---\n" + file_content + "\n";
                        std::cout << GREEN << "Loaded: " << read_path << " (" << file_content.size() << " bytes)" << RESET << "\n";
                        tool_output += "Read: " + read_path + "\n";
                    }
                    pos = end + 7;
                }

                // Process <run> commands
                pos = 0;
                while ((pos = response.find("<run>", pos)) != std::string::npos) {
                    size_t end = response.find("</run>", pos);
                    if (end == std::string::npos) break;
                    std::string cmd = response.substr(pos + 5, end - pos - 5);
                    std::cout << CYAN << "[Running: " << cmd << "]" << RESET << "\n";
                    std::string output = run_command(cmd);
                    std::cout << output << "\n";
                    tool_output += "Command " + cmd + ":\n" + output + "\n";
                    pos = end + 6;
                }

                // Process changes
                changes = parse_changes(response);
                for (const auto& change : changes) {
                    show_diff(change);
                    if (apply_change(change)) {
                        std::cout << GREEN << "✓ Applied" << RESET << "\n";
                    }
                }

                // Update turn history
                turn_history += "\nAssistant: " + response;
                if (!tool_output.empty()) {
                    turn_history += "\nTool Results:\n" + tool_output;
                }

                // Trim to avoid context overflow
                if (turn_history.length() > 6000) {
                    turn_history = turn_history.substr(turn_history.length() - 4000);
                }

                // Stop if no more tools used (LLM is done exploring)
                if (tool_output.empty() && changes.empty()) {
                    std::cout << response << "\n";
                    break;
                }
            }

            history += "User: " + input + "\nAssistant: [explored and processed]\n";
        } else if (changes.empty()) {
            // No tools and no changes - just print response
            std::cout << response << "\n";
            history += "User: " + input + "\nAssistant: " + response + "\n";
        } else {
            history += "User: " + input + "\nAssistant: [made code changes]\n";
        }

        // Trim history and context
        if (history.length() > 2000) {
            history = history.substr(history.length() - 1500);
        }
        if (context.length() > 4000) {
            context = context.substr(context.length() - 3000);
        }
    }
    
    curl_global_cleanup();
    std::cout << "\nGoodbye!\n";
    return 0;
}
