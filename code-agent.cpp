// code-agent.cpp
// Interactive coding agent with diff review

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
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
const std::string WORKSPACE = "/root/workspace";

bool is_in_workspace(const std::string& path) {
    // Resolve to absolute path and check it's in workspace
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) != nullptr) {
        return std::string(resolved).find(WORKSPACE) == 0;
    }
    // For new files that don't exist yet, check the path directly
    return path.find(WORKSPACE) == 0;
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
    return buffer.str();
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

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "[Error: Cannot execute command]";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    pclose(pipe);

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
const char* SYSTEM_PROMPT = R"(You are an expert coding assistant working in /root/workspace directory.

## TOOLS AVAILABLE

<list>/root/workspace/path</list>  - List directory contents
<read>/root/workspace/path/file</read>  - Read file contents
<run>shell command</run>  - Execute shell command (use for find, grep, tree, etc.)

## EDIT EXISTING FILE (must read first):
<change file="/root/workspace/path/file">
<description>what this does</description>
<old>exact text from file</old>
<new>replacement text</new>
</change>

## CREATE NEW FILE (old must be empty):
<change file="/root/workspace/path/newfile">
<description>creating new file</description>
<old></old>
<new>file contents here</new>
</change>

## CRITICAL RULES
1. ALWAYS <list> first to find what exists
2. ALWAYS <read> before editing - copy exact text into <old>
3. For NEW files: <old></old> MUST be empty
4. For EDITS: <old> must contain exact text copied from <read> output
5. NEVER guess paths - only use paths confirmed via <list>
6. If file not found, report what IS available

## CODEBASE ANALYSIS PROTOCOL

When asked to "analyze", "understand", or "examine" a codebase, follow this SYSTEMATIC approach:

### PHASE 1: Discovery (Structure Scan)
1. <run>find PROJECT_PATH -type f -name "*.md" 2>/dev/null | head -20</run>
2. <run>find PROJECT_PATH -name "package.json" -o -name "Makefile" -o -name "CMakeLists.txt" -o -name "Cargo.toml" -o -name "requirements.txt" -o -name "pyproject.toml" -o -name "go.mod" 2>/dev/null</run>
3. <run>find PROJECT_PATH -type f \( -name "*.cpp" -o -name "*.py" -o -name "*.js" -o -name "*.ts" -o -name "*.go" -o -name "*.rs" -o -name "*.java" -o -name "*.c" -o -name "*.h" \) 2>/dev/null | head -50</run>
4. <list>PROJECT_PATH</list>

### PHASE 2: Dependency Mapping
For EACH source file found:
1. <read>file</read>
2. Extract and document:
   - Import/include statements
   - Exported functions/classes
   - Internal vs external dependencies
   - File purpose and responsibilities

### PHASE 3: Architecture Understanding
Identify and document:
- Entry points (main functions, index files)
- Core modules and responsibilities
- Data flow between components
- Configuration files and purpose
- Build system details

### PHASE 4: Generate ANALYSIS.md
Create comprehensive analysis report with:
- Project overview and purpose
- Technology stack
- Directory structure with annotations
- Each component: file, purpose, key functions, dependencies
- Dependency graph showing file relationships
- Entry points and how to run
- Build instructions
- Key patterns and architectural notes

IMPORTANT: Read EVERY source file systematically. Do not skip files.
Track all imports to map dependencies. Be thorough and complete.
)";

// Analyze prompt template - %s gets replaced with project path
const char* ANALYZE_PROMPT_TEMPLATE = R"(ANALYSIS MODE ACTIVATED

Systematically analyze the codebase at: %s

Execute this analysis protocol step by step:

STEP 1 - DISCOVERY: Run these commands to map the project structure:
<run>find %s -type f -name "*.md" 2>/dev/null</run>
<run>find %s \( -name "package.json" -o -name "Makefile" -o -name "CMakeLists.txt" -o -name "Cargo.toml" -o -name "requirements.txt" -o -name "go.mod" \) 2>/dev/null</run>
<run>find %s -type f \( -name "*.cpp" -o -name "*.py" -o -name "*.js" -o -name "*.ts" -o -name "*.go" -o -name "*.h" -o -name "*.c" -o -name "*.rs" \) 2>/dev/null</run>

STEP 2 - READ BUILD FILES: Read any package.json, Makefile, etc. found
STEP 3 - READ DOCUMENTATION: Read README.md, CLAUDE.md if present
STEP 4 - READ EACH SOURCE FILE: Go through each source file systematically
STEP 5 - MAP DEPENDENCIES: Note which files import/include which
STEP 6 - GENERATE REPORT: Create %s/ANALYSIS.md with complete analysis

Begin with Step 1 discovery now.
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
    std::cout << "  " << YELLOW << "/analyze <path>" << RESET << " - Systematic codebase analysis\n";
    std::cout << "  " << YELLOW << "/file <path>" << RESET << "    - Load file into context\n";
    std::cout << "  " << YELLOW << "/clear" << RESET << "          - Clear context\n";
    std::cout << "  " << YELLOW << "/exit" << RESET << "           - Quit\n\n";
    
    while (true) {
        std::cout << BOLD << GREEN << ">>> " << RESET;
        std::string input;
        if (!std::getline(std::cin, input)) break;
        
        if (input.empty()) continue;
        
        // Commands
        if (input[0] == '/') {
            if (input == "/exit" || input == "/quit") break;
            
            if (input == "/clear") {
                context.clear();
                history.clear();
                std::cout << "Context cleared.\n";
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
                         path.c_str(), path.c_str(), path.c_str(), path.c_str(), path.c_str());

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

                    // Trim history if needed
                    if (analysis_history.length() > 6000) {
                        analysis_history = analysis_history.substr(analysis_history.length() - 4000);
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
        
        if (changes.empty()) {
            // No structured changes, just print response
            std::cout << response << "\n";
            history += "User: " + input + "\nAssistant: " + response + "\n";
            continue;
        }
        
        // Auto-apply all changes
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
        
        history += "User: " + input + "\nAssistant: [made code changes]\n";
        
        // Trim history
        if (history.length() > 4000) {
            history = history.substr(history.length() - 3000);
        }
    }
    
    curl_global_cleanup();
    std::cout << "\nGoodbye!\n";
    return 0;
}
