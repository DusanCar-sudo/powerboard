#pragma once
#include <vector>
#include <string>
#include <utility>

struct ProcessInfo {
    int pid;
    std::string name;
    double cpu_usage = 0.0;
    unsigned long long ram_kb = 0;
};

struct DriveInfo {
    std::string mount_point;
    unsigned long long total_gb = 0;
    unsigned long long used_gb = 0;
    double percent_used = 0.0;
};

struct SystemMetrics {
    double cpu_load = 0.0;
    double cpu_temp = 0.0;
    double cpu_freq = 0.0;
    double gpu_temp = 0.0;
    double gpu_load = 0.0;
    double current_watts = 0.0;
    double cumulative_kwh = 0.0;
    double accumulated_cost = 0.0;

    unsigned long long total_ram = 0, free_ram = 0, used_ram = 0;
    unsigned long long total_swap = 0, free_swap = 0, used_swap = 0;

    double net_download_kb = 0.0;
    double net_upload_kb = 0.0;
    double disk_read_kb = 0.0;
    double disk_write_kb = 0.0;

    double battery_percent = 100.0;
    std::string uptime_str = "0h 0m";

    std::vector<ProcessInfo> processes;
    std::vector<double> core_loads;
    std::vector<DriveInfo> drives;
};

struct GraphData {
    std::vector<double> watt_history;
    std::vector<double> cpu_load_history;
    std::vector<double> cpu_temp_history;
    std::vector<double> gpu_temp_history;
    std::vector<double> ram_history;
    std::vector<double> net_download_history;
    std::vector<double> gpu_load_history;
    const size_t max_size = 120;
};

struct Point3D {
    double x, y, z;
};

struct ClipItem {
    int id;
    std::string content;
    long long created_at;
};

struct AIConfig {
    std::string provider = "Ollama";
    std::string api_key = "";
    std::string model = "llama3";
    std::string api_url = "http://localhost:11434/api/chat";
};

struct BenchmarkResults {
    bool cpu_running = false;
    double cpu_single_mops = 0.0;
    double cpu_multi_mops = 0.0;
    double cpu_scaling = 0.0;
    int cpu_threads = 0;

    bool mem_running = false;
    double mem_read_gbps = 0.0;
    double mem_write_gbps = 0.0;
    double mem_copy_gbps = 0.0;

    bool disk_running = false;
    double disk_write_mbps = 0.0;
    double disk_read_mbps = 0.0;

    bool llm_running = false;
    double llm_bw = 0.0;
    std::vector<std::pair<std::string, double>> llm_estimates;

    std::string ollama_status = "Not checked";
    std::string lmstudio_status = "Not checked";

    int progress_pct = 0;
    std::string progress_label = "Idle";
};

enum Theme {
    NEON,
    WIREFRAME,
    DRACULA,
    CARBON,
    SRBIJA
};

constexpr double PRICE_PER_KWH = 0.15;
