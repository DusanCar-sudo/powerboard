#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>
#include <sqlite3.h>
#include "types.h"

inline std::string read_clipboard() {
    std::string result;
    FILE* pipe = popen("wl-paste -n 2>/dev/null", "r");
    if (!pipe) return "";
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

inline void write_clipboard(const std::string& text) {
    FILE* pipe = popen("wl-copy", "w");
    if (pipe) {
        fputs(text.c_str(), pipe);
        pclose(pipe);
    }
}

inline std::vector<ClipItem> get_clips(sqlite3* db) {
    std::vector<ClipItem> results;
    if (!db) return results;
    const char* select_sql = "SELECT id, content, created_at FROM clips ORDER BY id DESC LIMIT 15;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            std::string content = txt ? txt : "";
            long long created_at = sqlite3_column_int64(stmt, 2);
            results.push_back({id, content, created_at});
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

inline void add_clip(sqlite3* db, const std::string& content) {
    if (!db || content.empty()) return;
    const char* insert_sql = "INSERT OR IGNORE INTO clips (content, created_at) VALUES (?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db, "DELETE FROM clips WHERE id NOT IN (SELECT id FROM clips ORDER BY id DESC LIMIT 100);", nullptr, nullptr, nullptr);
}

inline void delete_clip(sqlite3* db, int id) {
    if (!db) return;
    const char* delete_sql = "DELETE FROM clips WHERE id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, delete_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}
