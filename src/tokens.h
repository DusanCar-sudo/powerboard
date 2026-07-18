#pragma once
#include <string>
#include <cstdint>
#include <cmath>
#include <mutex>

// Simple token estimator for offline usage
// Approximation: ~4 chars per token for English, ~3 for code, ~2.5 for non-Latin scripts
struct TokenStats {
    int64_t total_prompt_tokens = 0;
    int64_t total_completion_tokens = 0;
    int64_t total_tokens = 0;
    int32_t query_count = 0;
    mutable std::mutex mutex_;

    void reset() {
        std::lock_guard<std::mutex> lk(mutex_);
        total_prompt_tokens = 0;
        total_completion_tokens = 0;
        total_tokens = 0;
        query_count = 0;
    }

    int64_t estimate_tokens(const std::string& text) const {
        if (text.empty()) return 0;

        // Detect script type
        bool has_cyrillic = false;
        bool has_cjk = false;
        bool has_code = false;

        // Check for non-Latin characters
        for (char c : text) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc >= 0x80) {  // Non-ASCII
                // Simple Cyrillic detection (UTF-8)
                if (uc >= 0xD0 && uc <= 0xD9) has_cyrillic = true;
                // CJK range check (simplified)
                if (uc >= 0xE4 && uc <= 0xE9) has_cjk = true;
            }
        }

        // Code indicators
        has_code = (text.find("{") != std::string::npos ||
                    text.find("}") != std::string::npos ||
                    text.find(";") != std::string::npos ||
                    text.find("fn ") != std::string::npos ||
                    text.find("func") != std::string::npos ||
                    text.find("class ") != std::string::npos);

        // Estimate based on content type
        double chars_per_token;
        if (has_cjk) {
            chars_per_token = 1.5;  // CJK: more tokens per char
        } else if (has_cyrillic) {
            chars_per_token = 2.5;  // Cyrillic: moderate density
        } else if (has_code) {
            chars_per_token = 3.5;  // Code: denser than prose
        } else {
            chars_per_token = 4.0;  // English prose: standard
        }

        // Count actual characters (not bytes)
        size_t char_count = 0;
        for (size_t i = 0; i < text.size(); ) {
            unsigned char uc = static_cast<unsigned char>(text[i]);
            if (uc < 0x80) {
                i += 1;
            } else if ((uc & 0xE0) == 0xC0) {
                i += 2;
            } else if ((uc & 0xF0) == 0xE0) {
                i += 3;
            } else if ((uc & 0xF8) == 0xF0) {
                i += 4;
            } else {
                i += 1;  // Invalid UTF-8, skip
            }
            char_count++;
        }

        return static_cast<int64_t>(std::ceil(char_count / chars_per_token));
    }

    void record_query(int64_t prompt_tokens, int64_t completion_tokens) {
        std::lock_guard<std::mutex> lk(mutex_);
        total_prompt_tokens += prompt_tokens;
        total_completion_tokens += completion_tokens;
        total_tokens += prompt_tokens + completion_tokens;
        query_count++;
    }

    double estimate_cost_usd(const std::string& provider) const {
        std::lock_guard<std::mutex> lk(mutex_);
        // Rough cost estimates per 1M tokens (input + output average)
        double cost_per_million = 0.01;  // Default conservative

        if (provider == "OpenAI") {
            cost_per_million = 0.15;  // GPT-4o-mini: $0.15/$0.60 avg
        } else if (provider == "Gemini") {
            cost_per_million = 0.08;  // Gemini Flash
        } else if (provider == "DeepSeek") {
            cost_per_million = 0.03;  // DeepSeek-chat
        } else if (provider == "Ollama" || provider == "LM Studio") {
            cost_per_million = 0.0;   // Local: free
        }

        return (static_cast<double>(total_tokens) / 1e6) * cost_per_million;
    }

    int64_t get_remaining_context(const std::string& provider, const std::string& model) const {
        std::lock_guard<std::mutex> lk(mutex_);
        // Context window limits (conservative estimates)
        int64_t context_limit = 4096;  // Default

        if (provider == "Ollama" || provider == "LM Studio") {
            // Local models vary - use conservative estimate
            context_limit = 8192;
        } else if (provider == "OpenAI") {
            if (model.find("gpt-4") != std::string::npos) {
                context_limit = 128000;
            } else if (model.find("gpt-3.5") != std::string::npos) {
                context_limit = 16385;
            } else {
                context_limit = 128000;  // gpt-4o-mini default
            }
        } else if (provider == "Gemini") {
            context_limit = 1000000;  // Gemini 2.5 Flash
        } else if (provider == "DeepSeek") {
            context_limit = 64000;
        }

        // Reserve space for response (typically 25% of context)
        int64_t reserved = context_limit / 4;
        return context_limit - total_prompt_tokens - reserved;
    }

    std::string format_token_count(int64_t tokens) const {
        if (tokens < 1000) {
            return std::to_string(tokens);
        } else if (tokens < 1000000) {
            return std::to_string(tokens / 1000) + "K";
        } else {
            return std::to_string(tokens / 1000000) + "M";
        }
    }

    int64_t get_total_tokens() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return total_tokens;
    }

    int32_t get_query_count() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return query_count;
    }
};

// Global token stats instance
inline TokenStats g_token_stats;
