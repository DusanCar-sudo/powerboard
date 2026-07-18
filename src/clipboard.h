#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>
#include <sqlite3.h>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include "types.h"

// Thread-safe clipboard read with timeout to prevent Wayland hangs
inline std::string read_clipboard() {
    int pipefd[2];
    if (pipe(pipefd) == -1) return "";

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }

    if (pid == 0) {  // Child process
        close(pipefd[0]);  // Close read end
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        // Set stdin to /dev/null
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }

        execlp("wl-paste", "wl-paste", "-n", "2>/dev/null", nullptr);
        _exit(1);  // If exec fails
    }

    // Parent process
    close(pipefd[1]);  // Close write end

    std::string result;
    char buffer[256];
    ssize_t bytes_read;
    auto start_time = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(500);  // 500ms timeout

    // Make read end non-blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout) {
            kill(pid, SIGKILL);
            break;
        }

        bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            result += buffer;
        } else if (bytes_read == 0) {
            break;  // EOF
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            } else {
                break;  // Error
            }
        }
    }

    close(pipefd[0]);

    // Wait for child with timeout
    int status;
    waitpid(pid, &status, WNOHANG);  // Non-blocking wait
    kill(pid, SIGKILL);  // Ensure cleanup
    waitpid(pid, &status, 0);  // Final cleanup

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
