#pragma once
#include <cstdint>
#include <chrono>
#include <vector>
#include <thread>
#include <string>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include "types.h"

namespace fs = std::filesystem;

inline uint64_t mix_cpp(uint64_t x, uint64_t iters) {
    for (uint64_t i = 0; i < iters; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x = x * 0x2545F4914F6CDD1DULL;
    }
    return x;
}

inline double cpu_burn_cpp(double secs) {
    uint64_t chunk = 2000000;
    uint64_t x = 0x9E3779B97F4A7C15ULL;
    uint64_t done = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    while (true) {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - t0).count();
        if (elapsed >= secs) break;
        volatile uint64_t res = mix_cpp(x, chunk);
        x = res;
        done += chunk;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double real_elapsed = std::chrono::duration<double>(t1 - t0).count();
    return (real_elapsed > 0) ? (static_cast<double>(done) / real_elapsed / 1e6) : 0.0;
}

inline void run_cpu_test(BenchmarkResults& res, ftxui::ScreenInteractive& screen) {
    res.cpu_running = true;
    res.progress_pct = 5;
    res.progress_label = "Running single-core burn...";
    screen.PostEvent(ftxui::Event::Custom);

    res.cpu_single_mops = cpu_burn_cpp(1.2);

    res.progress_pct = 50;
    res.cpu_threads = std::thread::hardware_concurrency();
    res.progress_label = "Running multi-core burn (" + std::to_string(res.cpu_threads) + " threads)...";
    screen.PostEvent(ftxui::Event::Custom);

    std::vector<std::thread> pool;
    std::vector<double> scores(res.cpu_threads, 0.0);
    for (int i = 0; i < res.cpu_threads; ++i) {
        pool.push_back(std::thread([i, &scores]() {
            scores[i] = cpu_burn_cpp(1.2);
        }));
    }
    for (auto& t : pool) t.join();

    double sum = 0.0;
    for (double s : scores) sum += s;
    res.cpu_multi_mops = sum;
    res.cpu_scaling = (res.cpu_single_mops > 0) ? (res.cpu_multi_mops / res.cpu_single_mops) : 0.0;

    res.progress_pct = 100;
    res.progress_label = "CPU benchmark done!";
    res.cpu_running = false;
    screen.PostEvent(ftxui::Event::Custom);
}

inline void run_mem_test(BenchmarkResults& res, ftxui::ScreenInteractive& screen) {
    res.mem_running = true;
    res.progress_pct = 10;
    res.progress_label = "Allocating 256 MB buffer...";
    screen.PostEvent(ftxui::Event::Custom);

    size_t N = 16 * 1024 * 1024;
    double bytes = static_cast<double>(N * 8);

    std::vector<uint64_t> a(N, 0);
    std::vector<uint64_t> b(N, 0);

    res.progress_pct = 30;
    res.progress_label = "Running write pass...";
    screen.PostEvent(ftxui::Event::Custom);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < N; ++i) {
        a[i] = i;
    }
    volatile uint64_t* barrier_ptr = a.data();
    (void)barrier_ptr[N - 1];
    auto t1 = std::chrono::high_resolution_clock::now();
    double w_elapsed = std::chrono::duration<double>(t1 - t0).count();
    res.mem_write_gbps = bytes / w_elapsed / 1e9;

    res.progress_pct = 60;
    res.progress_label = "Running read pass...";
    screen.PostEvent(ftxui::Event::Custom);

    t0 = std::chrono::high_resolution_clock::now();
    uint64_t sum = 0;
    for (size_t i = 0; i < N; ++i) {
        sum += a[i];
    }
    volatile uint64_t sum_barrier = sum;
    (void)sum_barrier;
    t1 = std::chrono::high_resolution_clock::now();
    double r_elapsed = std::chrono::duration<double>(t1 - t0).count();
    res.mem_read_gbps = bytes / r_elapsed / 1e9;

    res.progress_pct = 80;
    res.progress_label = "Running copy pass...";
    screen.PostEvent(ftxui::Event::Custom);

    t0 = std::chrono::high_resolution_clock::now();
    std::copy(a.begin(), a.end(), b.begin());
    volatile uint64_t* b_barrier = b.data();
    (void)b_barrier[N - 1];
    t1 = std::chrono::high_resolution_clock::now();
    double c_elapsed = std::chrono::duration<double>(t1 - t0).count();
    res.mem_copy_gbps = (bytes * 2.0) / c_elapsed / 1e9;

    res.progress_pct = 100;
    res.progress_label = "Memory benchmark done!";
    res.mem_running = false;
    screen.PostEvent(ftxui::Event::Custom);
}

inline void run_disk_test(BenchmarkResults& res, ftxui::ScreenInteractive& screen) {
    res.disk_running = true;
    res.progress_pct = 10;
    res.progress_label = "Preparing write buffer (192 MB)...";
    screen.PostEvent(ftxui::Event::Custom);

    size_t block_size = 8 * 1024 * 1024;
    size_t num_blocks = 24;
    std::vector<uint8_t> block(block_size);
    for (size_t i = 0; i < block_size; ++i) {
        block[i] = static_cast<uint8_t>((i * 2654435761ULL) >> 16);
    }

    const char* home_c = getenv("HOME");
    if (!home_c) {
        res.progress_label = "Error: HOME not set";
        res.disk_running = false;
        screen.PostEvent(ftxui::Event::Custom);
        return;
    }
    std::string home(home_c);
    std::string path = home + "/.local/share/powerboard/bench.tmp";
    fs::create_directories(home + "/.local/share/powerboard");

    res.progress_pct = 20;
    res.progress_label = "Writing blocks...";
    screen.PostEvent(ftxui::Event::Custom);

    auto t0 = std::chrono::high_resolution_clock::now();
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        res.progress_label = "Disk error: could not write file";
        res.disk_running = false;
        screen.PostEvent(ftxui::Event::Custom);
        return;
    }

    for (size_t i = 0; i < num_blocks; ++i) {
        f.write(reinterpret_cast<const char*>(block.data()), block_size);
        res.progress_pct = 20 + static_cast<int>(30 * (static_cast<double>(i) / num_blocks));
        screen.PostEvent(ftxui::Event::Custom);
    }
    f.flush();
    int fd = open(path.c_str(), O_WRONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    f.close();
    auto t1 = std::chrono::high_resolution_clock::now();
    double w_elapsed = std::chrono::duration<double>(t1 - t0).count();
    res.disk_write_mbps = (static_cast<double>(block_size * num_blocks)) / w_elapsed / 1e6;

    res.progress_pct = 60;
    res.progress_label = "Reading blocks...";
    screen.PostEvent(ftxui::Event::Custom);

    t0 = std::chrono::high_resolution_clock::now();
    std::ifstream f_in(path, std::ios::binary);
    if (!f_in.is_open()) {
        res.progress_label = "Disk error: could not read file";
        res.disk_running = false;
        screen.PostEvent(ftxui::Event::Custom);
        return;
    }
    std::vector<uint8_t> read_buf(block_size);
    while (f_in.read(reinterpret_cast<char*>(read_buf.data()), block_size)) {
        volatile uint8_t* barrier = read_buf.data();
        (void)barrier[0];
    }
    f_in.close();
    t1 = std::chrono::high_resolution_clock::now();
    double r_elapsed = std::chrono::duration<double>(t1 - t0).count();
    res.disk_read_mbps = (static_cast<double>(block_size * num_blocks)) / r_elapsed / 1e6;

    fs::remove(path);

    res.progress_pct = 100;
    res.progress_label = "Disk benchmark done!";
    res.disk_running = false;
    screen.PostEvent(ftxui::Event::Custom);
}

inline void run_llm_test(BenchmarkResults& res, ftxui::ScreenInteractive& screen) {
    res.llm_running = true;
    res.progress_pct = 10;
    res.progress_label = "Measuring memory copy bandwidth...";
    screen.PostEvent(ftxui::Event::Custom);

    run_mem_test(res, screen);
    double bw = res.mem_copy_gbps > 0 ? res.mem_copy_gbps : 12.5;
    res.llm_bw = bw;

    double eff = bw * 0.72;
    res.llm_estimates.clear();
    res.llm_estimates.push_back({"1B q4 (0.75 GB)", eff / 0.75});
    res.llm_estimates.push_back({"3B q4 (2.00 GB)", eff / 2.0});
    res.llm_estimates.push_back({"7B q4 (4.40 GB)", eff / 4.4});
    res.llm_estimates.push_back({"13B q4 (7.90 GB)", eff / 7.9});
    res.llm_estimates.push_back({"34B q4 (19.5 GB)", eff / 19.5});

    res.progress_pct = 70;
    res.progress_label = "Probing local Ollama and LM Studio...";
    screen.PostEvent(ftxui::Event::Custom);

    FILE* p_ollama = popen("curl -s -m 2 -H \"User-Agent: Aura-Pulse/0.3.0\" http://127.0.0.1:11434/api/tags 2>/dev/null", "r");
    if (p_ollama) {
        std::string out; char buf[128];
        while (fgets(buf, sizeof(buf), p_ollama)) out += buf;
        pclose(p_ollama);
        if (out.find("models") != std::string::npos) {
            size_t m_pos = out.find("\"name\":\"");
            if (m_pos != std::string::npos) {
                size_t end_pos = out.find("\"", m_pos + 8);
                std::string model = out.substr(m_pos + 8, end_pos - (m_pos + 8));
                res.ollama_status = "ONLINE (" + model + ")";
            } else {
                res.ollama_status = "ONLINE (No models)";
            }
        } else {
            res.ollama_status = "OFFLINE";
        }
    } else {
        res.ollama_status = "OFFLINE";
    }

    FILE* p_lms = popen("curl -s -m 2 -H \"User-Agent: Aura-Pulse/0.3.0\" http://127.0.0.1:1234/v1/models 2>/dev/null", "r");
    if (p_lms) {
        std::string out; char buf[128];
        while (fgets(buf, sizeof(buf), p_lms)) out += buf;
        pclose(p_lms);
        if (out.find("\"id\"") != std::string::npos) {
            res.lmstudio_status = "ONLINE";
        } else {
            res.lmstudio_status = "OFFLINE";
        }
    } else {
        res.lmstudio_status = "OFFLINE";
    }

    res.progress_pct = 100;
    res.progress_label = "LLM estimator done!";
    res.llm_running = false;
    screen.PostEvent(ftxui::Event::Custom);
}

inline void probe_local_servers(BenchmarkResults& res, ftxui::ScreenInteractive& screen) {
    res.progress_pct = 50;
    res.progress_label = "Probing local API servers...";
    screen.PostEvent(ftxui::Event::Custom);

    FILE* p_ollama = popen("curl -s -m 2 -H \"User-Agent: Aura-Pulse/0.3.0\" http://127.0.0.1:11434/api/tags 2>/dev/null", "r");
    if (p_ollama) {
        std::string out; char buf[128];
        while (fgets(buf, sizeof(buf), p_ollama)) out += buf;
        pclose(p_ollama);
        if (out.find("models") != std::string::npos) {
            size_t m_pos = out.find("\"name\":\"");
            if (m_pos != std::string::npos) {
                size_t end_pos = out.find("\"", m_pos + 8);
                res.ollama_status = "ONLINE (" + out.substr(m_pos + 8, end_pos - (m_pos + 8)) + ")";
            } else {
                res.ollama_status = "ONLINE (No models)";
            }
        } else { res.ollama_status = "OFFLINE"; }
    } else { res.ollama_status = "OFFLINE"; }

    FILE* p_lms = popen("curl -s -m 2 -H \"User-Agent: Aura-Pulse/0.3.0\" http://127.0.0.1:1234/v1/models 2>/dev/null", "r");
    if (p_lms) {
        std::string out; char buf[128];
        while (fgets(buf, sizeof(buf), p_lms)) out += buf;
        pclose(p_lms);
        if (out.find("\"id\"") != std::string::npos) res.lmstudio_status = "ONLINE";
        else res.lmstudio_status = "OFFLINE";
    } else { res.lmstudio_status = "OFFLINE"; }

    res.progress_pct = 100;
    res.progress_label = "Probes completed!";
    screen.PostEvent(ftxui::Event::Custom);
}
