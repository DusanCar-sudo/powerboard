#pragma once
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include "types.h"
#include "utils.h"

namespace fs = std::filesystem;

inline void save_ai_config(const AIConfig& config) {
    const char* home_c = getenv("HOME");
    if (!home_c) return;
    std::string home(home_c);
    std::string path = home + "/.config/powerboard/ai_config.txt";
    fs::create_directories(home + "/.config/powerboard");
    std::ofstream f(path);
    if (f.is_open()) {
        f << "provider=" << config.provider << "\n";
        f << "api_key=" << config.api_key << "\n";
        f << "model=" << config.model << "\n";
        f << "api_url=" << config.api_url << "\n";
    }
}

inline AIConfig load_ai_config() {
    AIConfig config;
    const char* home_c = getenv("HOME");
    if (!home_c) return config;
    std::string home(home_c);
    std::string path = home + "/.config/powerboard/ai_config.txt";
    std::ifstream f(path);
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                if (key == "provider") config.provider = val;
                else if (key == "api_key") config.api_key = val;
                else if (key == "model") config.model = val;
                else if (key == "api_url") config.api_url = val;
            }
        }
    }
    return config;
}

inline std::string run_ai_query(const AIConfig& config, const std::string& system_prompt, const std::string& user_prompt) {
    std::string escaped_sys = escape_json_string(system_prompt);
    std::string escaped_user = escape_json_string(user_prompt);

    std::string url;
    std::string payload;
    std::string parse_script;
    std::string headers = "-H \"Content-Type: application/json\" ";

    if (config.provider == "Ollama") {
        url = config.api_url.empty() ? "http://localhost:11434/api/chat" : config.api_url;
        payload = "{\"model\":\"" + config.model + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + escaped_sys + "\"},{\"role\":\"user\",\"content\":\"" + escaped_user + "\"}],\"stream\":false}";
        parse_script = "import sys, json; print(json.load(sys.stdin)['message']['content'])";
    }
    else if (config.provider == "Gemini") {
        url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=" + config.api_key;
        payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + escaped_sys + "\\n\\n" + escaped_user + "\"}]}]}";
        parse_script = "import sys, json; print(json.load(sys.stdin)['candidates'][0]['content']['parts'][0]['text'])";
    }
    else if (config.provider == "OpenAI" || config.provider == "DeepSeek" || config.provider == "LM Studio") {
        if (config.provider == "OpenAI") {
            url = "https://api.openai.com/v1/chat/completions";
            headers += "-H \"Authorization: Bearer " + config.api_key + "\" ";
        } else if (config.provider == "DeepSeek") {
            url = "https://api.deepseek.com/v1/chat/completions";
            headers += "-H \"Authorization: Bearer " + config.api_key + "\" ";
        } else {
            url = "http://localhost:1234/v1/chat/completions";
        }
        std::string m = config.model;
        if (m.empty()) {
            m = (config.provider == "OpenAI") ? "gpt-4o-mini" : ((config.provider == "DeepSeek") ? "deepseek-chat" : "lmstudio-model");
        }
        payload = "{\"model\":\"" + m + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + escaped_sys + "\"},{\"role\":\"user\",\"content\":\"" + escaped_user + "\"}],\"temperature\":0.7}";
        parse_script = "import sys, json; print(json.load(sys.stdin)['choices'][0]['message']['content'])";
    }

    const char* home_c = getenv("HOME");
    if (!home_c) return "Error: HOME not set";
    std::string home(home_c);
    std::string temp_payload_path = home + "/.local/share/powerboard/payload.json";
    fs::create_directories(home + "/.local/share/powerboard");
    std::ofstream f(temp_payload_path);
    if (!f.is_open()) return "Error: Cannot write payload.json";
    f << payload;
    f.close();

    std::string cmd = "curl -s -m 30 " + headers + "-d @" + temp_payload_path + " \"" + url + "\" | python3 -c \"" + parse_script + "\" 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        fs::remove(temp_payload_path);
        return "Error: Could not run curl pipe";
    }

    std::string res;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) res += buf;
    pclose(pipe);

    fs::remove(temp_payload_path);

    if (res.find("Traceback") != std::string::npos || res.find("KeyError") != std::string::npos) {
        return "AI Error: Failed to parse response or model is unreachable.\nCheck your API Key, settings, and network connection.";
    }

    return res;
}
