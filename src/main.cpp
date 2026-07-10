#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <csignal>
#include <unistd.h>
#include <mutex>
#include <sstream>
#include <sqlite3.h>

#include <ftxui/screen/terminal.hpp>
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/canvas.hpp"

#include "types.h"
#include "utils.h"
#include "datalogger.h"
#include "clipboard.h"
#include "ai.h"
#include "benchmarks.h"
#include "scanner.h"

using namespace ftxui;
using namespace std;
namespace fs = std::filesystem;

// --- SYSTEM MUTEXES FOR SAFE THREAD CONCURRENCY ---
std::mutex vault_mutex;
std::mutex bench_mutex;

// Collects all spawned threads for clean shutdown
static std::vector<std::thread> bg_threads;

int main() {
    auto screen = ScreenInteractive::TerminalOutput();
    SystemMetrics metrics;
    GraphData graph;
    AdvancedHardwareScanner scanner;
    DataLogger logger;

    // Persistently declared canvases to avoid stack-use-after-scope
    Canvas cpu_canvas(10, 10);
    Canvas memory_canvas(10, 10);
    Canvas power_canvas(10, 10);
    Canvas thermals_canvas(10, 10);
    Canvas network_canvas(10, 10);
    Canvas gpu_canvas(10, 10);
    Canvas globe_canvas(56, 56);

    // Dynamic core count detection
    unsigned int threads = thread::hardware_concurrency();
    metrics.core_loads.assign(threads > 0 ? threads : 16, 0.0);

    // Initial synchronous seed call
    scanner.update_all_metrics(metrics, 0.1);

    std::mutex metrics_mutex;
    int selected_proc_idx = 0;
    Theme current_theme = NEON;

    // --- NEW MULTI-TAB NAVIGATION AND AUXILIARY STATES ---
    int current_tab = 0;
    BenchmarkResults bench_res;
    AIConfig ai_config = load_ai_config();

    // SQLite Database initialisation
    sqlite3* vault_db = nullptr;
    string last_seen_clip;
    {
        const char* home_c = getenv("HOME");
        string home_dir = home_c ? home_c : "/tmp";
        fs::create_directories(home_dir + "/.local/share/powerboard");
        string db_path = home_dir + "/.local/share/powerboard/vault.db";
        if (sqlite3_open(db_path.c_str(), &vault_db) == SQLITE_OK) {
            const char* sql = "CREATE TABLE IF NOT EXISTS clips (id INTEGER PRIMARY KEY AUTOINCREMENT, content TEXT UNIQUE, created_at INTEGER NOT NULL);";
            sqlite3_exec(vault_db, sql, nullptr, nullptr, nullptr);
        }
    }

    int selected_bench_menu = 0;
    int selected_opt_menu = 0;
    int selected_vault_idx = 0;
    int selected_ai_menu = 0;
    int vault_focus_pane = 0; // 0: Vault (Left), 1: AI Settings (Right)
    string custom_question;
    string ai_response_text = "Select a clipboard item on the left, configure your AI provider on the right, and choose an action recipe.";
    bool ai_thinking = false;

    // Pre-generate 180 points on a Fibonacci 3D sphere for the live spinning globe
    vector<Point3D> globe_points;
    for (int i = 0; i < 180; ++i) {
        double phi = acos(1.0 - 2.0 * (i + 0.5) / 180.0);
        double theta = M_PI * (1.0 + sqrt(5.0)) * i;
        globe_points.push_back({sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi)});
    }

    double globe_angle_y = 0.0;
    double globe_angle_x = 0.0;

    auto component = Container::Vertical({});
    component = CatchEvent(component, [&](Event event) {
        if (event == Event::Custom) return true;
        string input = event.input();

        // Detect if user is actively typing in a text field (Tab 3, right pane, editable items)
        bool in_text_input = (current_tab == 3 && vault_focus_pane == 1 &&
                              (selected_ai_menu == 1 || selected_ai_menu == 2 || selected_ai_menu == 3));

        // Global Tab Navigation — use FTXUI event constants for function keys
        if (event == Event::F1) { current_tab = 0; return true; }
        if (event == Event::F2) { current_tab = 1; return true; }
        if (event == Event::F3) { current_tab = 2; return true; }
        if (event == Event::F4) { current_tab = 3; return true; }
        if ((input == "[" || input == "{") && !in_text_input) {
            current_tab = (current_tab + 3) % 4;
            return true;
        }
        if ((input == "]" || input == "}") && !in_text_input) {
            current_tab = (current_tab + 1) % 4;
            return true;
        }

        // Quit only when NOT actively typing in a text field
        if ((input == "q" || input == "\x1B") && !in_text_input) { screen.ExitLoopClosure()(); return true; }

        // --- TAB 0: DIAGNOSTICS CONTROL ---
        if (current_tab == 0) {
            if (event == Event::ArrowDown) {
                selected_proc_idx = min(9, selected_proc_idx + 1);
                return true;
            }
            if (event == Event::ArrowUp) {
                selected_proc_idx = max(0, selected_proc_idx - 1);
                return true;
            }
            if (input == "k" || input == "K") {
                int pid_to_kill = -1;
                {
                    std::lock_guard<std::mutex> lock(metrics_mutex);
                    if (selected_proc_idx < static_cast<int>(metrics.processes.size())) {
                        pid_to_kill = metrics.processes[selected_proc_idx].pid;
                    }
                }
                if (pid_to_kill != -1) {
                    kill(pid_to_kill, SIGKILL);
                }
                return true;
            }
        }
        // --- TAB 1: BENCHMARKS CONTROL ---
        else if (current_tab == 1) {
            if (event == Event::ArrowDown) {
                selected_bench_menu = min(4, selected_bench_menu + 1);
                return true;
            }
            if (event == Event::ArrowUp) {
                selected_bench_menu = max(0, selected_bench_menu - 1);
                return true;
            }
            if (event == Event::Return) {
                std::lock_guard<std::mutex> lk(bench_mutex);
                if (selected_bench_menu == 0 && !bench_res.cpu_running) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        run_cpu_test(bench_res, screen);
                    });
                } else if (selected_bench_menu == 1 && !bench_res.mem_running) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        run_mem_test(bench_res, screen);
                    });
                } else if (selected_bench_menu == 2 && !bench_res.disk_running) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        run_disk_test(bench_res, screen);
                    });
                } else if (selected_bench_menu == 3 && !bench_res.llm_running) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        run_llm_test(bench_res, screen);
                    });
                } else if (selected_bench_menu == 4) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        probe_local_servers(bench_res, screen);
                    });
                }
                return true;
            }
        }
        // --- TAB 2: SYSTEM OPTIMIZATION CONTROL ---
        else if (current_tab == 2) {
            if (event == Event::ArrowDown) {
                selected_opt_menu = min(7, selected_opt_menu + 1);
                return true;
            }
            if (event == Event::ArrowUp) {
                selected_opt_menu = max(0, selected_opt_menu - 1);
                return true;
            }
            if (event == Event::Return) {
                if (selected_opt_menu == 0) {
                    bg_threads.emplace_back([]() { set_power_profile("balanced"); });
                } else if (selected_opt_menu == 1) {
                    bg_threads.emplace_back([]() { set_power_profile("performance"); });
                } else if (selected_opt_menu == 2) {
                    bg_threads.emplace_back([]() { set_power_profile("power-saver"); });
                } else if (selected_opt_menu == 3) {
                    bool current_boost = get_cpu_boost();
                    bg_threads.emplace_back([current_boost]() { set_cpu_boost_privileged(!current_boost); });
                } else if (selected_opt_menu == 4) {
                    int sw = get_swappiness();
                    bg_threads.emplace_back([sw]() { set_swappiness_privileged(min(100, sw + 10)); });
                } else if (selected_opt_menu == 5) {
                    int sw = get_swappiness();
                    bg_threads.emplace_back([sw]() { set_swappiness_privileged(max(0, sw - 10)); });
                } else if (selected_opt_menu == 6) {
                    bg_threads.emplace_back([&screen]() {
                        run_balance_cores_privileged();
                        screen.PostEvent(Event::Custom);
                    });
                } else if (selected_opt_menu == 7) {
                    bg_threads.emplace_back([&screen]() {
                        run_drop_caches_privileged();
                        screen.PostEvent(Event::Custom);
                    });
                }
                return true;
            }
        }
        // --- TAB 3: CLIPBOARD VAULT & AI ENRICHMENT ---
        else if (current_tab == 3) {
            if (event == Event::ArrowLeft) {
                vault_focus_pane = 0;
                return true;
            }
            if (event == Event::ArrowRight) {
                vault_focus_pane = 1;
                return true;
            }

            if (vault_focus_pane == 0) {
                if (event == Event::ArrowDown) {
                    selected_vault_idx = min(14, selected_vault_idx + 1);
                    return true;
                }
                if (event == Event::ArrowUp) {
                    selected_vault_idx = max(0, selected_vault_idx - 1);
                    return true;
                }
                if (event == Event::Return) {
                    std::lock_guard<std::mutex> lk(vault_mutex);
                    auto clips = get_clips(vault_db);
                    if (selected_vault_idx < static_cast<int>(clips.size())) {
                        write_clipboard(clips[selected_vault_idx].content);
                        last_seen_clip = clips[selected_vault_idx].content;
                    }
                    return true;
                }
                if (input == "d" || input == "D") {
                    std::lock_guard<std::mutex> lk(vault_mutex);
                    auto clips = get_clips(vault_db);
                    if (selected_vault_idx < static_cast<int>(clips.size())) {
                        delete_clip(vault_db, clips[selected_vault_idx].id);
                    }
                    return true;
                }
            } else {
                if (event == Event::ArrowDown) {
                    selected_ai_menu = min(7, selected_ai_menu + 1);
                    return true;
                }
                if (event == Event::ArrowUp) {
                    selected_ai_menu = max(0, selected_ai_menu - 1);
                    return true;
                }

                if (selected_ai_menu == 0) {
                    if (event == Event::Return || input == " ") {
                        if (ai_config.provider == "Ollama") ai_config.provider = "OpenAI";
                        else if (ai_config.provider == "OpenAI") ai_config.provider = "DeepSeek";
                        else if (ai_config.provider == "DeepSeek") ai_config.provider = "Gemini";
                        else if (ai_config.provider == "Gemini") ai_config.provider = "LM Studio";
                        else ai_config.provider = "Ollama";
                        save_ai_config(ai_config);
                        return true;
                    }
                }
                else if (selected_ai_menu == 1) {
                    if (event == Event::Backspace) {
                        if (!ai_config.model.empty()) ai_config.model.pop_back();
                        save_ai_config(ai_config);
                        return true;
                    }
                    else if (input.size() == 1 && isprint(input[0])) {
                        ai_config.model += input;
                        save_ai_config(ai_config);
                        return true;
                    }
                }
                else if (selected_ai_menu == 2) {
                    if (event == Event::Backspace) {
                        if (!ai_config.api_key.empty()) ai_config.api_key.pop_back();
                        save_ai_config(ai_config);
                        return true;
                    }
                    else if (input.size() == 1 && isprint(input[0])) {
                        ai_config.api_key += input;
                        save_ai_config(ai_config);
                        return true;
                    }
                }
                else if (selected_ai_menu == 3) {
                    if (event == Event::Backspace) {
                        if (!custom_question.empty()) custom_question.pop_back();
                        return true;
                    }
                    else if (input.size() == 1 && isprint(input[0])) {
                        custom_question += input;
                        return true;
                    }
                }
                else if (event == Event::Return) {
                    string system_prompt;
                    string user_prompt;

                    string clip_text;
                    {
                        std::lock_guard<std::mutex> lk(vault_mutex);
                        auto clips = get_clips(vault_db);
                        if (selected_vault_idx < static_cast<int>(clips.size())) {
                            clip_text = clips[selected_vault_idx].content;
                        }
                    }

                    if (selected_ai_menu == 4) {
                        system_prompt = "You are a helpful AI Linux coding copilot named Aura AI. Answer the user's question directly, clearly, and concisely in a technical cyberpunk tone.";
                        user_prompt = custom_question;
                        if (!clip_text.empty()) {
                            user_prompt += "\n\nContext Clipboard Text:\n" + clip_text;
                        }
                    } else if (selected_ai_menu == 5) {
                        system_prompt = "Summarize the following text concisely. Provide a bulleted list of key takeaways, and a one-sentence overview.";
                        user_prompt = clip_text;
                    } else if (selected_ai_menu == 6) {
                        system_prompt = "You are an expert software security and performance auditor. Analyze the following source code clip. Identify potential performance improvements, security concerns, or logic errors, and output extremely clean improved code.";
                        user_prompt = clip_text;
                    } else if (selected_ai_menu == 7) {
                        system_prompt = "Translate the following text. If it is in Serbian (Cyrillic or Latin), translate it to English. If it is in English, translate it to Serbian Cyrillic. Maintain a high-quality, professional, contextually appropriate translation.";
                        user_prompt = clip_text;
                    }

                    if (!user_prompt.empty()) {
                        ai_thinking = true;
                        ai_response_text = "Thinking...";
                        screen.PostEvent(Event::Custom);

                        bg_threads.emplace_back([&ai_config, system_prompt, user_prompt, &ai_response_text, &ai_thinking, &screen]() {
                            ai_response_text = run_ai_query(ai_config, system_prompt, user_prompt);
                            ai_thinking = false;
                            screen.PostEvent(Event::Custom);
                        });
                    }
                    return true;
                }
            }
        }

        // Direct theme hotkeys matching sidebar index
        if (input == "1") { current_theme = NEON; return true; }
        if (input == "2") { current_theme = WIREFRAME; return true; }
        if (input == "3") { current_theme = DRACULA; return true; }
        if (input == "4") { current_theme = CARBON; return true; }
        if (input == "5") { current_theme = SRBIJA; return true; }
        if (input == "t" || input == "T") {
            current_theme = static_cast<Theme>((static_cast<int>(current_theme) + 1) % 5);
            return true;
        }
        return false;
    });

    bool refresh_ui_loop = true;
    thread data_thread([&]() {
        auto last_update_time = chrono::high_resolution_clock::now();
        auto last_log_time = chrono::high_resolution_clock::now();
        while (refresh_ui_loop) {
            auto current_time = chrono::high_resolution_clock::now();
            chrono::duration<double> elapsed = current_time - last_update_time;
            last_update_time = current_time;

            SystemMetrics temp_metrics;
            {
                std::lock_guard<std::mutex> lock(metrics_mutex);
                scanner.update_all_metrics(metrics, elapsed.count());

                metrics.cumulative_kwh += (metrics.current_watts * (elapsed.count() / 3600.0)) / 1000.0;
                metrics.accumulated_cost = metrics.cumulative_kwh * PRICE_PER_KWH;

                graph.watt_history.push_back(metrics.current_watts);
                if (graph.watt_history.size() > graph.max_size) graph.watt_history.erase(graph.watt_history.begin());

                graph.cpu_load_history.push_back(metrics.cpu_load);
                if (graph.cpu_load_history.size() > graph.max_size) graph.cpu_load_history.erase(graph.cpu_load_history.begin());

                graph.cpu_temp_history.push_back(metrics.cpu_temp);
                if (graph.cpu_temp_history.size() > graph.max_size) graph.cpu_temp_history.erase(graph.cpu_temp_history.begin());

                graph.gpu_temp_history.push_back(metrics.gpu_temp);
                if (graph.gpu_temp_history.size() > graph.max_size) graph.gpu_temp_history.erase(graph.gpu_temp_history.begin());

                graph.ram_history.push_back(metrics.used_ram);
                if (graph.ram_history.size() > graph.max_size) graph.ram_history.erase(graph.ram_history.begin());

                graph.net_download_history.push_back(metrics.net_download_kb);
                if (graph.net_download_history.size() > graph.max_size) graph.net_download_history.erase(graph.net_download_history.begin());

                double busy_pct = metrics.cpu_load * 0.85;
                metrics.gpu_load = busy_pct;
                graph.gpu_load_history.push_back(metrics.gpu_load);
                if (graph.gpu_load_history.size() > graph.max_size) graph.gpu_load_history.erase(graph.gpu_load_history.begin());

                // Increment globe spinning physics matrix
                globe_angle_y += 0.04;
                globe_angle_x += 0.02;

                temp_metrics = metrics;
            }

            static int clipboard_poll_counter = 0;
            if (++clipboard_poll_counter >= 5) {
                clipboard_poll_counter = 0;
                string clip = read_clipboard();
                if (!clip.empty() && clip != last_seen_clip) {
                    last_seen_clip = clip;
                    std::lock_guard<std::mutex> lock(vault_mutex);
                    add_clip(vault_db, clip);
                }
            }

            if (chrono::duration_cast<chrono::seconds>(current_time - last_log_time).count() >= 5) {
                log_to_csv(logger, temp_metrics.current_watts, temp_metrics.cumulative_kwh, temp_metrics.accumulated_cost);
                last_log_time = current_time;
            }

            screen.PostEvent(Event::Custom);
            this_thread::sleep_for(chrono::milliseconds(100));
        }
    });

    auto renderer = Renderer(component, [&] {
        SystemMetrics local_metrics;
        vector<double> local_watt_history;
        vector<double> local_cpu_load_history;
        vector<double> local_cpu_temp_history;
        vector<double> local_gpu_temp_history;
        vector<double> local_ram_history;
        vector<double> local_net_download_history;
        vector<double> local_gpu_load_history;
        double local_globe_angle_y, local_globe_angle_x;
        {
            std::lock_guard<std::mutex> lock(metrics_mutex);
            local_metrics = metrics;
            local_watt_history = graph.watt_history;
            local_cpu_load_history = graph.cpu_load_history;
            local_cpu_temp_history = graph.cpu_temp_history;
            local_gpu_temp_history = graph.gpu_temp_history;
            local_ram_history = graph.ram_history;
            local_net_download_history = graph.net_download_history;
            local_gpu_load_history = graph.gpu_load_history;
            local_globe_angle_y = globe_angle_y;
            local_globe_angle_x = globe_angle_x;
        }

        auto term_size = Terminal::Size();
        int term_w = term_size.dimx;

        // Dynamic theme styling tokens
        Color primary_color = Color::Cyan;
        Color secondary_color = Color::Magenta;
        Color accent_color = Color::Yellow;
        Color border_color = Color::Cyan;
        Color bg_selected = Color::Red;

        if (current_theme == NEON) {
            primary_color = Color::Cyan;
            secondary_color = Color::Magenta;
            accent_color = Color::Yellow;
            border_color = Color::Cyan;
            bg_selected = Color::Red;
        } else if (current_theme == WIREFRAME) {
            primary_color = Color::GrayLight;
            secondary_color = Color::GrayLight;
            accent_color = Color::White;
            border_color = Color::GrayLight;
            bg_selected = Color::GrayLight;
        } else if (current_theme == DRACULA) {
            primary_color = Color::RGB(189, 147, 249);
            secondary_color = Color::RGB(255, 85, 85);
            accent_color = Color::RGB(255, 121, 198);
            border_color = Color::RGB(189, 147, 249);
            bg_selected = Color::RGB(98, 114, 164);
        } else if (current_theme == CARBON) {
            primary_color = Color::Green;
            secondary_color = Color::GrayLight;
            accent_color = Color::GreenLight;
            border_color = Color::GrayDark;
            bg_selected = Color::Green;
        } else if (current_theme == SRBIJA) {
            primary_color = Color::Blue;
            secondary_color = Color::Red;
            accent_color = Color::White;
            border_color = Color::White;
            bg_selected = Color::BlueLight;
        }

        // Inline Cyrillic Translation helper
        auto L = [&](const string& en, const string& sr) {
            return ::L(en, sr, current_theme);
        };

        // --- SIDEBAR WIDGETS (Rotating globe + resume details) ---
        int g_size = 56;
        globe_canvas = Canvas(g_size, g_size);
        double r_globe = g_size / 2.3;
        double cx_g = g_size / 2.0;
        double cy_g = g_size / 2.0;
        double cos_y = cos(local_globe_angle_y);
        double sin_y = sin(local_globe_angle_y);
        double cos_x = cos(local_globe_angle_x);
        double sin_x = sin(local_globe_angle_x);

        // Draw spinning 3D network connections (mesh) on the globe
        for (size_t i = 0; i < globe_points.size(); ++i) {
            for (size_t j = i + 1; j < globe_points.size(); ++j) {
                double dx = globe_points[i].x - globe_points[j].x;
                double dy = globe_points[i].y - globe_points[j].y;
                double dz = globe_points[i].z - globe_points[j].z;
                double dist = sqrt(dx*dx + dy*dy + dz*dz);
                if (dist < 0.28) {
                    double xi = globe_points[i].x * cos_y - globe_points[i].z * sin_y;
                    double zi = globe_points[i].x * sin_y + globe_points[i].z * cos_y;
                    double yi = globe_points[i].y * cos_x - zi * sin_x;
                    double zzi = globe_points[i].y * sin_x + zi * cos_x;

                    double xj = globe_points[j].x * cos_y - globe_points[j].z * sin_y;
                    double zj = globe_points[j].x * sin_y + globe_points[j].z * cos_y;
                    double yj = globe_points[j].y * cos_x - zj * sin_x;
                    double zzj = globe_points[j].y * sin_x + zj * cos_x;

                    int sxi = static_cast<int>(cx_g + xi * r_globe);
                    int syi = static_cast<int>(cy_g + yi * r_globe);
                    int sxj = static_cast<int>(cx_g + xj * r_globe);
                    int syj = static_cast<int>(cy_g + yj * r_globe);

                    Color col_mesh = (zzi > 0 && zzj > 0) ? primary_color : Color::GrayDark;
                    globe_canvas.DrawPointLine(sxi, syi, sxj, syj, col_mesh);
                }
            }
        }
        // Draw globe dots
        for (const auto& pt : globe_points) {
            double x1 = pt.x * cos_y - pt.z * sin_y;
            double z1 = pt.x * sin_y + pt.z * cos_y;
            double y2 = pt.y * cos_x - z1 * sin_x;
            double z2 = pt.y * sin_x + z1 * cos_x;

            int sx = static_cast<int>(cx_g + x1 * r_globe);
            int sy = static_cast<int>(cy_g + y2 * r_globe);

            Color col_dot = (z2 > 0) ? primary_color : Color::RGB(10, 30, 80);
            globe_canvas.DrawPoint(sx, sy, true, col_dot);
        }

        auto globe_widget = vbox({
            canvas(&globe_canvas) | size(HEIGHT, EQUAL, 14) | size(WIDTH, EQUAL, 28) | hcenter,
            text(L("AURA POWERBOARD", "АУРА ПОТРОШЊА")) | bold | hcenter | color(primary_color)
        });

        auto navigation_box = vbox({
            text("  " + L("NAVIGATION", "НАВИГАЦИЈА")) | bold | color(primary_color),
            separator(),
            text(current_tab == 0 ? "  [F1] * " + L("DIAGNOSTICS", "ДИЈАГНОСТИКА") : "  [F1]   " + L("DIAGNOSTICS", "ДИЈАГНОСТИКА")) | color(current_tab == 0 ? primary_color : Color::White),
            text(current_tab == 1 ? "  [F2] * " + L("BENCHMARKS", "ТЕСТИРАЊЕ") : "  [F2]   " + L("BENCHMARKS", "ТЕСТИРАЊЕ")) | color(current_tab == 1 ? primary_color : Color::White),
            text(current_tab == 2 ? "  [F3] * " + L("OPTIMIZATION", "ОПТИМИЗАЦИЈА") : "  [F3]   " + L("OPTIMIZATION", "ОПТИМИЗАЦИЈА")) | color(current_tab == 2 ? primary_color : Color::White),
            text(current_tab == 3 ? "  [F4] * " + L("VAULT & AI", "БЕЛЕЖНИК И АИ") : "  [F4]   " + L("VAULT & AI", "БЕЛЕЖНИК И АИ")) | color(current_tab == 3 ? primary_color : Color::White),
        });

        auto status_resume = vbox({
            text("  " + L("STATUS", "СТАТУС")) | bold | color(primary_color),
            separator(),
            hbox({ text("  " + L("Draw:", "Потрошња: ")), filler(), text(format_double(local_metrics.current_watts, 1) + " W  ") }),
            hbox({ text("  " + L("Energy:", "Енергија: ")), filler(), text(format_double(local_metrics.cumulative_kwh, 4) + " kWh  ") }),
            hbox({ text("  " + L("System:", "Систем: ")), filler(), text(L("Operational", "Оперативан") + "  ") | color(Color::Green) })
        });

        auto theme_selector = vbox({
            text("  " + L("THEME", "ТЕМА")) | bold | color(primary_color),
            separator(),
            text(current_theme == NEON ? "  [1] * NEON" : "  [1] NEON") | color(current_theme == NEON ? primary_color : Color::White),
            text(current_theme == WIREFRAME ? "  [2] * WIREFRAME" : "  [2] WIREFRAME") | color(current_theme == WIREFRAME ? primary_color : Color::White),
            text(current_theme == DRACULA ? "  [3] * DRACULA" : "  [3] DRACULA") | color(current_theme == DRACULA ? primary_color : Color::White),
            text(current_theme == CARBON ? "  [4] * CARBON" : "  [4] CARBON") | color(current_theme == CARBON ? primary_color : Color::White),
            text(current_theme == SRBIJA ? "  [5] * ЋИРИЛИЦА" : "  [5] ЋИРИЛИЦА") | color(current_theme == SRBIJA ? primary_color : Color::White),
        });

        auto sidebar = vbox({ globe_widget, separator(), navigation_box, separator(), status_resume, separator(), theme_selector }) | border;

        // --- DASHBOARD GRIDS (Right Side Panel) ---
        int right_w = std::max(80, term_w - 32);
        int curve_w_px = (right_w * 0.6) * 2;
        int curve_h_px = 32;

        // 1. CPU MATRIX Box
        cpu_canvas = Canvas(curve_w_px, curve_h_px);
        if (local_cpu_load_history.size() > 1) {
            for (size_t i = 1; i < local_cpu_load_history.size(); ++i) {
                int x1 = static_cast<int>((static_cast<double>(i - 1) / (graph.max_size - 1)) * (curve_w_px - 1));
                int x2 = static_cast<int>((static_cast<double>(i) / (graph.max_size - 1)) * (curve_w_px - 1));
                int max_y = curve_h_px - 4;
                int y1 = static_cast<int>((local_cpu_load_history[i - 1] / 100.0) * max_y);
                int y2 = static_cast<int>((local_cpu_load_history[i] / 100.0) * max_y);
                for (int x = x1; x <= x2; ++x) {
                    double t = (x2 == x1) ? 0.0 : static_cast<double>(x - x1) / (x2 - x1);
                    int y = static_cast<int>(y1 + t * (y2 - y1));
                    for (int curr_y = 0; curr_y <= y; ++curr_y) {
                        cpu_canvas.DrawPoint(x, (curve_h_px - 2) - curr_y, true, primary_color);
                    }
                }
            }
        }

        Elements cpu_core_elems;
        for (size_t i = 0; i < local_metrics.core_loads.size(); ++i) {
            double load = local_metrics.core_loads[i];
            Color col = primary_color;
            if (load > 75) col = Color::Red;
            else if (load > 40) col = Color::Orange1;
            string num_str = to_string(i);
            if (num_str.size() == 1) num_str = "0" + num_str;
            cpu_core_elems.push_back(text(num_str) | border | color(col));
        }
        Elements cpu_grid_rows;
        Elements temp_row;
        for (size_t i = 0; i < cpu_core_elems.size(); ++i) {
            temp_row.push_back(cpu_core_elems[i]);
            if (temp_row.size() == 8 || i == cpu_core_elems.size() - 1) {
                cpu_grid_rows.push_back(hbox(move(temp_row)));
                temp_row.clear();
            }
        }

        auto cpu_matrix_box = window(text("  " + L("CPU MATRIX", "ПРОЦЕСОРСКА МАТРИЦА") + "  " + format_double(local_metrics.cpu_load, 1) + "% ") | bold, vbox({
            hbox({
                vbox({
                    hbox({ text(" " + L("FREQ:", "ФРЕКВ:") + " ") | bold, text(format_double(local_metrics.cpu_freq, 2) + " GHz ") | color(primary_color) }),
                    hbox({ text(" " + L("TEMP:", "ТЕМП:") + " ") | bold, text(to_string(static_cast<int>(local_metrics.cpu_temp)) + " °C ") | color(Color::Orange1) }),
                    hbox({ text(" " + L("LOAD:", "ОПТЕРЕЋ:") + " ") | bold, text(format_double(local_metrics.cpu_load / 20.0, 2) + " ") })
                }),
                separator(),
                vbox(move(cpu_grid_rows)) | flex
            }),
            separator(),
            canvas(&cpu_canvas) | size(HEIGHT, EQUAL, 8)
        })) | color(border_color) | flex;

        // 2. MEMORY BANKS Box
        int mem_w_px = (right_w * 0.35) * 2;
        memory_canvas = Canvas(mem_w_px, curve_h_px);
        if (local_ram_history.size() > 1) {
            for (size_t i = 1; i < local_ram_history.size(); ++i) {
                int x1 = static_cast<int>((static_cast<double>(i - 1) / (graph.max_size - 1)) * (mem_w_px - 1));
                int x2 = static_cast<int>((static_cast<double>(i) / (graph.max_size - 1)) * (mem_w_px - 1));
                int max_y = curve_h_px - 4;
                double pct1 = static_cast<double>(local_ram_history[i - 1]) / local_metrics.total_ram;
                double pct2 = static_cast<double>(local_ram_history[i]) / local_metrics.total_ram;
                int y1 = static_cast<int>(pct1 * max_y);
                int y2 = static_cast<int>(pct2 * max_y);
                for (int x = x1; x <= x2; ++x) {
                    double t = (x2 == x1) ? 0.0 : static_cast<double>(x - x1) / (x2 - x1);
                    int y = static_cast<int>(y1 + t * (y2 - y1));
                    for (int curr_y = 0; curr_y <= y; ++curr_y) {
                        memory_canvas.DrawPoint(x, (curve_h_px - 2) - curr_y, true, secondary_color);
                    }
                }
            }
        }

        auto memory_banks_box = window(text("  " + L("MEMORY BANKS", "МЕМОРИЈСКЕ БАНКЕ") + "  ") | bold, vbox({
            hbox({ text(" " + L("RAM:", "РАМ:") + " "), filler(), text(to_string(local_metrics.used_ram) + " / " + to_string(local_metrics.total_ram) + " MB") }),
            hbox({ text(" " + L("SWAP:", "СВАП:") + " "), filler(), text(to_string(local_metrics.used_swap) + " / " + to_string(local_metrics.total_swap) + " MB") }),
            hbox({ text(" " + L("FREE:", "СЛОБ:") + " "), filler(), text(to_string(local_metrics.free_ram) + " MB") }),
            separator(),
            canvas(&memory_canvas) | size(HEIGHT, EQUAL, 8)
        })) | color(border_color);

        // 3. POWER DRAW Box
        power_canvas = Canvas(curve_w_px, curve_h_px);
        if (local_watt_history.size() > 1) {
            for (size_t i = 1; i < local_watt_history.size(); ++i) {
                int x1 = static_cast<int>((static_cast<double>(i - 1) / (graph.max_size - 1)) * (curve_w_px - 1));
                int x2 = static_cast<int>((static_cast<double>(i) / (graph.max_size - 1)) * (curve_w_px - 1));
                int max_y = curve_h_px - 4;
                int y1 = static_cast<int>((local_watt_history[i - 1] / 60.0) * max_y);
                int y2 = static_cast<int>((local_watt_history[i] / 60.0) * max_y);
                for (int x = x1; x <= x2; ++x) {
                    double t = (x2 == x1) ? 0.0 : static_cast<double>(x - x1) / (x2 - x1);
                    int y = static_cast<int>(y1 + t * (y2 - y1));
                    for (int curr_y = 0; curr_y <= y; ++curr_y) {
                        power_canvas.DrawPoint(x, (curve_h_px - 2) - curr_y, true, primary_color);
                    }
                }
            }
        }

        auto power_draw_box = window(text("  " + L("POWER DRAW", "ПОТРОШЊА СТРУЈЕ") + "  " + format_double(local_metrics.current_watts, 1) + " W ") | bold, vbox({
            hbox({
                vbox({
                    hbox({ text(" " + L("APU PKG:", "АПУ ПАКЕТ:") + " ") | bold, text(format_double(local_metrics.current_watts * 0.6, 1) + " W ") | color(primary_color) }),
                    hbox({ text(" " + L("PROFILE:", "РЕЖИМ:") + " ") | bold, text("performance ") | color(accent_color) }),
                    hbox({ text(" " + L("CHARGE:", "НАПУЊ:") + " ") | bold, text(to_string(static_cast<int>(local_metrics.battery_percent)) + "% ") })
                }),
                separator(),
                vbox({
                    hbox({ text(" " + L("CONSUMPTION:", "ПОТРОШЊА:") + " ") | bold, text(format_double(local_metrics.current_watts, 1) + " W") }),
                    hbox({ text(" " + L("ENRG TOTAL:", "УКУПНО ЕН:") + " ") | bold, text(format_double(local_metrics.cumulative_kwh, 4) + " kWh") }),
                    hbox({ text(" " + L("EST COST:", "ПРОЦЕНА ТРОШКА:") + " ") | bold, text("$" + format_double(local_metrics.accumulated_cost, 4)) })
                })
            }),
            separator(),
            canvas(&power_canvas) | size(HEIGHT, EQUAL, 8)
        })) | color(border_color) | flex;

        // 4. THERMALS Box (Circular Gauges)
        thermals_canvas = Canvas(mem_w_px, 32);
        int cx1 = static_cast<int>(mem_w_px * 0.18);
        int cx2 = static_cast<int>(mem_w_px * 0.50);
        int cx3 = static_cast<int>(mem_w_px * 0.82);
        int cy_c = 16;
        int r_c = 8;
        draw_arc(thermals_canvas, cx1, cy_c, r_c, local_metrics.cpu_temp, Color::Red);
        draw_arc(thermals_canvas, cx2, cy_c, r_c, local_metrics.gpu_temp, Color::Orange1);
        draw_arc(thermals_canvas, cx3, cy_c, r_c, local_metrics.battery_percent, Color::Green);

        auto thermals_box = window(text("  " + L("THERMALS", "ТЕМПЕРАТУРЕ") + "  ") | bold, vbox({
            hbox({
                text(" " + L("CPU:", "ПРОЦ:") + " " + to_string(static_cast<int>(local_metrics.cpu_temp)) + "°C") | color(Color::Red) | hcenter | flex,
                text(" " + L("GPU:", "ГРАФ:") + " " + to_string(static_cast<int>(local_metrics.gpu_temp)) + "°C") | color(Color::Orange1) | hcenter | flex,
                text(" " + L("BAT:", "БАТ:") + " " + to_string(static_cast<int>(local_metrics.battery_percent)) + "%") | color(Color::Green) | hcenter | flex
            }),
            separator(),
            canvas(&thermals_canvas) | size(HEIGHT, EQUAL, 8)
        })) | color(border_color);

        // 5. NETWORK LINK Box
        int col3_w_px = (right_w / 3) * 2;
        network_canvas = Canvas(col3_w_px, curve_h_px);
        if (local_net_download_history.size() > 1) {
            for (size_t i = 1; i < local_net_download_history.size(); ++i) {
                int x1 = static_cast<int>((static_cast<double>(i - 1) / (graph.max_size - 1)) * (col3_w_px - 1));
                int x2 = static_cast<int>((static_cast<double>(i) / (graph.max_size - 1)) * (col3_w_px - 1));
                int max_y = curve_h_px - 4;
                int y1 = static_cast<int>((local_net_download_history[i - 1] / 100.0) * max_y);
                int y2 = static_cast<int>((local_net_download_history[i] / 100.0) * max_y);
                for (int x = x1; x <= x2; ++x) {
                    double t = (x2 == x1) ? 0.0 : static_cast<double>(x - x1) / (x2 - x1);
                    int y = static_cast<int>(y1 + t * (y2 - y1));
                    for (int curr_y = 0; curr_y <= y; ++curr_y) {
                        network_canvas.DrawPoint(x, (curve_h_px - 2) - curr_y, true, secondary_color);
                    }
                }
            }
        }

        auto network_link_box = window(text("  " + L("NETWORK LINK", "МРЕЖНА ВЕЗА") + "  ") | bold, vbox({
            hbox({ text(" " + L("DOWN:", "ПРИЈЕМ:") + " "), text(format_double(local_metrics.net_download_kb, 1) + " KB/s") | color(primary_color) }),
            hbox({ text(" " + L("UP:", "СЛАЊЕ:") + "   "), text(format_double(local_metrics.net_upload_kb, 1) + " KB/s") | color(secondary_color) }),
            separator(),
            canvas(&network_canvas) | size(HEIGHT, EQUAL, 6)
        })) | color(border_color) | flex;

        // 6. STORAGE ARRAY Box
        Elements drive_progress_rows;
        for (const auto& drv : local_metrics.drives) {
            int filled = static_cast<int>(drv.percent_used / 10.0);
            string bar = "[";
            for (int k = 0; k < 10; ++k) {
                bar += (k < filled) ? "■" : " ";
            }
            bar += "] " + format_double(drv.percent_used, 0) + "%";

            string path_cut = drv.mount_point;
            if (path_cut.size() > 11) path_cut = path_cut.substr(0, 9) + "..";

            drive_progress_rows.push_back(
                hbox({
                    text(" " + path_cut) | size(WIDTH, EQUAL, 12),
                    text(bar) | color(primary_color),
                    filler(),
                    text(to_string(drv.used_gb) + "G/" + to_string(drv.total_gb) + "G ")
                })
            );
        }

        auto storage_array_box = window(text("  " + L("STORAGE ARRAY", "СКЛАДИШНИ СИСТЕМ") + "  ") | bold, vbox({
            hbox({ text(" " + L("READ:", "ЧИТАЊЕ:") + "  "), text(format_double(local_metrics.disk_read_kb / 1024.0, 1) + " MB/s") | color(primary_color) }),
            hbox({ text(" " + L("WRITE:", "УПИС:") + " "), text(format_double(local_metrics.disk_write_kb / 1024.0, 1) + " MB/s") | color(secondary_color) }),
            separator(),
            vbox(move(drive_progress_rows))
        })) | color(border_color) | flex;

        // 7. GPU CORE Box
        gpu_canvas = Canvas(col3_w_px, curve_h_px);
        if (local_gpu_load_history.size() > 1) {
            for (size_t i = 1; i < local_gpu_load_history.size(); ++i) {
                int x1 = static_cast<int>((static_cast<double>(i - 1) / (graph.max_size - 1)) * (col3_w_px - 1));
                int x2 = static_cast<int>((static_cast<double>(i) / (graph.max_size - 1)) * (col3_w_px - 1));
                int max_y = curve_h_px - 4;
                int y1 = static_cast<int>((local_gpu_load_history[i - 1] / 100.0) * max_y);
                int y2 = static_cast<int>((local_gpu_load_history[i] / 100.0) * max_y);
                for (int x = x1; x <= x2; ++x) {
                    double t = (x2 == x1) ? 0.0 : static_cast<double>(x - x1) / (x2 - x1);
                    int y = static_cast<int>(y1 + t * (y2 - y1));
                    for (int curr_y = 0; curr_y <= y; ++curr_y) {
                        gpu_canvas.DrawPoint(x, (curve_h_px - 2) - curr_y, true, Color::Purple);
                    }
                }
            }
        }

        auto gpu_core_box = window(text("  " + L("GPU CORE", "ГРАФИЧКО ЈЕЗГРО") + "  " + format_double(local_metrics.gpu_load, 0) + "% ") | bold, vbox({
            hbox({ text(" " + L("VRAM:", "ВРАМ:") + " "), text("1.6 / 2.0 GB") | color(primary_color) }),
            hbox({ text(" " + L("TEMP:", "ТЕМП:") + " "), text(to_string(static_cast<int>(local_metrics.gpu_temp)) + " °C") | color(accent_color) }),
            separator(),
            canvas(&gpu_canvas) | size(HEIGHT, EQUAL, 6)
        })) | color(border_color) | flex;

        // --- RIGHT SIDE PANEL TABS DISPATCHER ---
        Element right_panel;

        if (current_tab == 0) {
            // DIAGNOSTICS TAB
            auto row1 = hbox({ cpu_matrix_box, memory_banks_box | size(WIDTH, EQUAL, right_w * 0.35) });
            auto row2 = hbox({ power_draw_box, thermals_box | size(WIDTH, EQUAL, right_w * 0.35) });
            auto row3 = hbox({ network_link_box, storage_array_box, gpu_core_box });

            Elements proc_rows;
            proc_rows.push_back(hbox({
                text("  PID") | bold | size(WIDTH, EQUAL, 10),
                text(L("PROCESS NAME", "НАЗИВ ПРОЦЕСА")) | bold,
                filler(),
                text(L("MEM RES  ", "МЕМОРИЈА  ")) | bold
            }) | bgcolor(Color::GrayDark));

            for (size_t i = 0; i < local_metrics.processes.size(); ++i) {
                const auto& p = local_metrics.processes[i];
                string ram_str = to_string(p.ram_kb / 1024) + " MB";
                string name_str = p.name;
                if (name_str.size() > 35) name_str = name_str.substr(0, 32) + "...";

                auto row = hbox({
                    text("  " + to_string(p.pid)) | size(WIDTH, EQUAL, 10),
                    text(name_str),
                    filler(),
                    text(ram_str + "  ")
                });
                if (static_cast<int>(i) == selected_proc_idx) {
                    row = row | bgcolor(primary_color) | color(Color::White);
                }
                proc_rows.push_back(row);
            }

            auto process_box = window(text("  " + L("Active Task Kernel (Top RAM)", "Активни Системски Процеси (Топ РАМ)") + "  ") | bold, vbox(move(proc_rows))) | color(border_color) | flex;

            auto header = hbox({
                text("  " + L("AURA POWERBOARD DIAGNOSTICS", "АУРА ДИЈАГНОСТИКА ПОТРОШЊЕ") + "  ") | bold | color(primary_color),
                text(" " + L("real-time telemetry · 10 Hz", "телеметрија у реалном времену · 10 Hz") + " ") | color(Color::GrayDark),
                filler(),
                text("  [T/t] " + L("Cycle Theme", "Промена Теме") + "  ") | color(accent_color)
            });

            right_panel = vbox({ header, row1, row2, row3, process_box });
        }
        else if (current_tab == 1) {
            // BENCHMARKS TAB — snapshot bench_res under lock
            BenchmarkResults local_bench;
            {
                std::lock_guard<std::mutex> lk(bench_mutex);
                local_bench = bench_res;
            }

            auto header = hbox(
                text("  " + L("AURA COCKPIT BENCHMARKS", "АУРА ТЕСТИРАЊЕ СИСТЕМА") + "  ") | bold | color(primary_color),
                text(" " + L("measure bare-metal performance", "мерите перформансе хардвера") + " ") | color(Color::GrayDark)
            );

            Element progress_bar = filler();
            if (local_bench.cpu_running || local_bench.mem_running || local_bench.disk_running || local_bench.llm_running) {
                int filled = local_bench.progress_pct / 5;
                string bar;
                for (int k = 0; k < 20; ++k) bar += (k < filled) ? "█" : "░";
                progress_bar = hbox(
                    text("  " + local_bench.progress_label + "  ") | bold | color(accent_color),
                    text("[" + bar + "] " + to_string(local_bench.progress_pct) + "%") | color(primary_color)
                ) | border;
            } else {
                progress_bar = hbox(
                    text("  " + L("Ready to run benchmark. Press [Enter] on selected option.", "Спреман за покретање. Притисните [Enter] на изабраној опцији.") + "  ") | color(Color::Green)
                ) | border;
            }

            auto make_menu_item = [&](int idx, const string& label) -> Element {
                bool is_sel = (idx == selected_bench_menu);
                auto prefix = is_sel ? "  [Enter] * " : "            ";
                auto col = is_sel ? primary_color : Color::White;
                return text(prefix + label) | color(col) | bold;
            };

            auto action_list = window(text("  " + L("SELECT TEST", "ОДАБЕРИТЕ ТЕСТ") + "  ") | bold, vbox(
                make_menu_item(0, L("Run CPU Burn (Bitwise Mixer MOPS)", "Покрени CPU Burn (Битвајз Миксер MOPS)")),
                separator(),
                make_menu_item(1, L("Run Memory Bandwidth (256MB Alloc)", "Покрени Меморијски Проток (256MB)")),
                separator(),
                make_menu_item(2, L("Run Disk Throughput (192MB Temp)", "Покрени Проток Диска (192MB Темп)")),
                separator(),
                make_menu_item(3, L("Run LLM Tokens Estimation", "Процени Брзину Локалног LLM-а")),
                separator(),
                make_menu_item(4, L("Probe Ollama / LM Studio Server", "Скенирај Ollama / LM Studio Сервер"))
            )) | color(border_color) | size(WIDTH, EQUAL, 48);

            Elements results_rows;
            results_rows.push_back(text("  " + L("BENCHMARK TELEMETRY ARCHIVE", "АРХИВА РЕЗУЛТАТА ТЕСТИРАЊА") + "  ") | bold | color(accent_color));
            results_rows.push_back(separator());

            results_rows.push_back(hbox(
                text("  " + L("CPU Single-Core Core Burn:", "Једнојезгарно CPU Сагоревање:") + " "),
                filler(),
                text(local_bench.cpu_single_mops > 0 ? format_double(local_bench.cpu_single_mops, 2) + " MOP/s" : "N/A") | bold | color(primary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("CPU Multi-Core Core Burn:", "Вишејезгарно CPU Сагоревање:") + " "),
                filler(),
                text(local_bench.cpu_multi_mops > 0 ? format_double(local_bench.cpu_multi_mops, 2) + " MOP/s" : "N/A") | bold | color(primary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("CPU Multi-Thread Scaling:", "CPU Вишени Задати Опсег:") + " "),
                filler(),
                text(local_bench.cpu_scaling > 0 ? format_double(local_bench.cpu_scaling, 2) + "x (" + to_string(local_bench.cpu_threads) + " " + L("Cores", "Језгара") + ")" : "N/A") | bold | color(accent_color)
            ));
            results_rows.push_back(separator());

            results_rows.push_back(hbox(
                text("  " + L("RAM Write Throughput:", "РАМ Проток Уписа:") + "      "),
                filler(),
                text(local_bench.mem_write_gbps > 0 ? format_double(local_bench.mem_write_gbps, 2) + " GB/s" : "N/A") | bold | color(secondary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("RAM Read Throughput:", "РАМ Проток Читања:") + "       "),
                filler(),
                text(local_bench.mem_read_gbps > 0 ? format_double(local_bench.mem_read_gbps, 2) + " GB/s" : "N/A") | bold | color(secondary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("RAM Copy/Triad Bandwidth:", "РАМ Копирање/Триад Проток:") + "  "),
                filler(),
                text(local_bench.mem_copy_gbps > 0 ? format_double(local_bench.mem_copy_gbps, 2) + " GB/s" : "N/A") | bold | color(accent_color)
            ));
            results_rows.push_back(separator());

            results_rows.push_back(hbox(
                text("  " + L("Disk Sequential Write:", "Диск Узастопни Упис:") + "     "),
                filler(),
                text(local_bench.disk_write_mbps > 0 ? format_double(local_bench.disk_write_mbps, 1) + " MB/s" : "N/A") | bold | color(primary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("Disk Sequential Read:", "Диск Узастопно Читање:") + "      "),
                filler(),
                text(local_bench.disk_read_mbps > 0 ? format_double(local_bench.disk_read_mbps, 1) + " MB/s" : "N/A") | bold | color(primary_color)
            ));
            results_rows.push_back(separator());

            results_rows.push_back(text("  " + L("PROJECTED LOCAL AI SPEEDS (CPU INFERENCE)", "ПРОЈЕКТОВАНЕ БРЗИНЕ ЛОКАЛНОГ АИ-ја (CPU)") + "  ") | bold | color(accent_color));
            if (local_bench.llm_estimates.empty()) {
                results_rows.push_back(text("  " + L("Run LLM Tokens Estimation to calculate speeds...", "Покрени процену LLM токена за израчунавање брзине...")) | color(Color::GrayDark));
            } else {
                for (const auto& est : local_bench.llm_estimates) {
                    results_rows.push_back(hbox(
                        text("   - " + est.first + ": "),
                        filler(),
                        text(format_double(est.second, 1) + " tok/s") | bold | color(Color::Green)
                    ));
                }
            }
            results_rows.push_back(separator());

            results_rows.push_back(hbox(
                text("  " + L("Local Ollama API status:", "Статус локалног Ollama API-ја:") + "   "),
                filler(),
                text(local_bench.ollama_status) | bold | color(local_bench.ollama_status.find("ONLINE") != string::npos ? Color::Green : Color::Red)
            ));
            results_rows.push_back(hbox(
                text("  " + L("LM Studio Server status:", "Статус LM Studio сервера:") + "   "),
                filler(),
                text(local_bench.lmstudio_status) | bold | color(local_bench.lmstudio_status == "ONLINE" ? Color::Green : Color::Red)
            ));

            auto results_box = window(text("  " + L("TEST RESULTS ARCHIVE", "АРХИВА РЕЗУЛТАТА") + "  ") | bold, vbox(move(results_rows))) | color(border_color) | flex;

            right_panel = vbox(header, progress_bar, hbox(action_list, results_box) | flex);
        }
        else if (current_tab == 2) {
            // SYSTEM OPTIMIZATION TAB
            auto header = hbox(
                text("  " + L("AURA SYSTEM OPTIMIZER", "АУРА ОПТИМИЗАЦИЈА СИСТЕМА") + "  ") | bold | color(primary_color),
                text(" " + L("fine-tune OS settings for maximum efficiency", "прилагодите подешавања оперативног система") + " ") | color(Color::GrayDark)
            );

            string active_profile = get_power_profile();
            bool active_boost = get_cpu_boost();
            int swappiness_val = get_swappiness();

            auto make_opt_item = [&](int idx, const string& title, const string& current_val, const string& action_label) -> Element {
                bool is_sel = (idx == selected_opt_menu);
                auto prefix = is_sel ? "  [Enter] * " : "            ";
                auto col = is_sel ? primary_color : Color::White;
                return vbox(
                    hbox(
                        text(prefix + title) | bold | color(col),
                        filler(),
                        text(current_val) | bold | color(accent_color)
                    ),
                    hbox(
                        text("            " + action_label) | color(Color::GrayLight)
                    ),
                    separator()
                );
            };

            auto tuning_list = window(text("  " + L("OS TUNING INTERFACES (REQUIRES POLKIT/PKEXEC)", "ИНТЕРФЕЈСИ ЗА ПРИЛАГОЂАВАЊЕ СИСТЕМА (ТРАЖИ СУДО)") + "  ") | bold, vbox(
                make_opt_item(0, L("Power Daemon: Balanced Profile", "Режим потрошње: Балансирано"), active_profile == "balanced" ? "ACTIVE" : "INACTIVE", L("Press [Enter] to switch power profiles daemon to Balanced.", "Притисните [Enter] да пребаците на балансиран режим потрошње.")),
                make_opt_item(1, L("Power Daemon: Performance Profile", "Режим потрошње: Перформансе"), active_profile == "performance" ? "ACTIVE" : "INACTIVE", L("Press [Enter] to switch power profiles daemon to Performance.", "Притисните [Enter] да пребаците на режим високих перформанси.")),
                make_opt_item(2, L("Power Daemon: Power Saver Profile", "Режим потрошње: Штедња батерије"), active_profile == "power-saver" ? "ACTIVE" : "INACTIVE", L("Press [Enter] to switch power profiles daemon to Power Saver.", "Притисните [Enter] да пребаците на режим штедње батерије.")),
                make_opt_item(3, L("CPU Core Turbo Boost", "CPU Турбо Буст"), active_boost ? "ENABLED" : "DISABLED", L("Press [Enter] to toggle dynamic CPU core clock boost.", "Притисните [Enter] да промените динамичко убрзање CPU језгара.")),
                make_opt_item(4, L("Linux Swappiness: Increase (+10)", "Линукс Свапинес: Повећај (+10)"), to_string(swappiness_val) + " / 100", L("Press [Enter] to increase swappiness threshold.", "Притисните [Enter] да повећате границу сваповања.")),
                make_opt_item(5, L("Linux Swappiness: Decrease (-10)", "Линукс Свапинес: Смањи (-10)"), to_string(swappiness_val) + " / 100", L("Press [Enter] to decrease swappiness threshold.", "Притисните [Enter] да смањите границу сваповања.")),
                make_opt_item(6, L("Rebalance & Online All CPU Cores", "Ребалансирање и Буђење CPU Језгара"), "SPREAD ON", L("Press [Enter] to run kernel online validator and spread hardware IRQ interrupts.", "Притисните [Enter] да покренете валидацију језгара и ребалансирате прекиде.")),
                make_opt_item(7, L("Drop OS RAM Page Cache", "Ослободи Системски РАМ Кеш"), "SYNC", L("Press [Enter] to safely sync storage queues and dump buffered caches.", "Притисните [Enter] да безбедно синхронизујете уписе и ослободите кеш меморију."))
            )) | color(border_color) | flex;

            right_panel = vbox(header, tuning_list);
        }
        else if (current_tab == 3) {
            // CLIPBOARD VAULT & AI TAB
            auto header = hbox(
                text("  " + L("AURA CLIPBOARD VAULT & AI ENRICHMENT", "АУРА СЕФ БЕЛЕЖНИК И АИ СЕРВИСИ") + "  ") | bold | color(primary_color),
                text(" " + L("cyberpunk clipboard manager and smart copilot", "сигурносни бележник и АИ ко-пилот") + " ") | color(Color::GrayDark)
            );

            Elements vault_rows;
            vector<ClipItem> clips;
            {
                std::lock_guard<std::mutex> lk(vault_mutex);
                clips = get_clips(vault_db);
            }

            if (clips.empty()) {
                vault_rows.push_back(text("  " + L("No clipboard content logged yet.", "Свеска је тренутно празна.") + "  ") | color(Color::GrayDark));
            } else {
                for (size_t i = 0; i < clips.size(); ++i) {
                    string s = clips[i].content;
                    std::replace(s.begin(), s.end(), '\n', ' ');
                    if (s.size() > 36) s = s.substr(0, 33) + "...";
                    auto prefix = (static_cast<int>(i) == selected_vault_idx) ? " * " : "   ";
                    auto col = (static_cast<int>(i) == selected_vault_idx) ? ((vault_focus_pane == 0) ? primary_color : Color::White) : Color::White;
                    vault_rows.push_back(text(prefix + s) | color(col) | bold);
                }
            }
            auto vault_box = window(text("  " + L("CLIPBOARD CORES [ArrowLeft]", "СЕФ КЛИПБОРД БЕЛЕЖНИЦА") + "  ") | bold, vbox(move(vault_rows))) | color(border_color) | size(WIDTH, EQUAL, 42);

            auto make_ai_input_row = [&](int idx, const string& label, const string& val) -> Element {
                bool is_sel = (idx == selected_ai_menu) && (vault_focus_pane == 1);
                auto prefix = is_sel ? " * " : "   ";
                auto col = is_sel ? primary_color : Color::White;
                return hbox(
                    text(prefix + label) | bold | color(col),
                    text(val) | color(accent_color) | bold
                );
            };

            auto make_recipe_row = [&](int idx, const string& label) -> Element {
                bool is_sel = (idx == selected_ai_menu) && (vault_focus_pane == 1);
                auto prefix = is_sel ? " [Enter] -> " : "            ";
                auto col = is_sel ? primary_color : Color::White;
                return text(prefix + label) | color(col) | bold;
            };

            auto ai_inputs = vbox(
                make_ai_input_row(0, L("Provider: ", "АИ Сервер: "), ai_config.provider),
                separator(),
                make_ai_input_row(1, L("Model:    ", "АИ Модел:  "), ai_config.model + " (type to edit)"),
                separator(),
                make_ai_input_row(2, L("API Key:  ", "АПИ Кључ:  "), ai_config.api_key.empty() ? "NONE" : string(std::min(static_cast<size_t>(10), ai_config.api_key.size()), '*') + " (type to edit)"),
                separator(),
                make_ai_input_row(3, L("Prompt:   ", "Питање:    "), custom_question + "_"),
                separator(),
                make_recipe_row(4, L("Submit Custom Prompt", "Пошаљи сопствени упит")),
                separator(),
                make_recipe_row(5, L("AI Recipe: Summarize Active Clip", "АИ Рецепт: Скрати активну белешку")),
                separator(),
                make_recipe_row(6, L("AI Recipe: Audit Code Clip", "АИ Рецепт: Ревидирај програмски код")),
                separator(),
                make_recipe_row(7, L("AI Recipe: Translate SRB/ENG", "АИ Рецепт: Преведи српски/енглески"))
            );

            auto ai_config_box = window(text("  " + L("AURA AI COPILOT [ArrowRight]", "КОНФИГУРАЦИЈА АИ-ја") + "  ") | bold, ai_inputs) | color(border_color) | size(HEIGHT, EQUAL, 21);

            Elements resp_wrapped;
            size_t max_char = right_w - 46;
            string current_word;
            size_t line_len = 0;
            for (char c : ai_response_text) {
                if (c == '\n') {
                    resp_wrapped.push_back(text(current_word));
                    current_word.clear();
                    line_len = 0;
                } else if (c == ' ') {
                    if (line_len + current_word.size() > max_char) {
                        resp_wrapped.push_back(text(current_word));
                        current_word.clear();
                        line_len = 0;
                    } else {
                        current_word += c;
                        line_len += current_word.size();
                    }
                } else {
                    current_word += c;
                }
            }
            if (!current_word.empty()) {
                resp_wrapped.push_back(text(current_word));
            }

            auto ai_response_box = window(text("  " + L("CO-PILOT ANSWERS FEED", "ОДГОВОРИ АИ КО-ПИЛОТА") + "  ") | bold, vbox(move(resp_wrapped)) | vscroll_indicator | frame) | color(border_color) | flex;

            right_panel = vbox(header, hbox(vault_box, vbox(ai_config_box, ai_response_box) | flex) | flex);
        }

        auto footer = hbox({
            text(" " + L("Navigation:", "Навигација:") + " ") | bold,
        });

        if (current_tab == 0) {
            footer = hbox({
                text(" " + L("Navigation:", "Навигација:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Process", "Одабир процеса") + " "),
                text(" [k] ") | bgcolor(Color::Red) | bold, text(" " + L("SIGKILL Process", "Принудно Заустави") + " "),
                text(" [1-5] ") | bgcolor(Color::GrayDark), text(" " + L("Switch Theme", "Избор теме") + " "),
                filler(),
                text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
            });
        } else if (current_tab == 1) {
            footer = hbox({
                text(" " + L("Navigation:", "Навигација:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Test", "Одабир теста") + " "),
                text(" [Enter] ") | bgcolor(Color::Green) | bold, text(" " + L("Run Selected Test", "Покрени тест") + " "),
                filler(),
                text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
            });
        } else if (current_tab == 2) {
            footer = hbox({
                text(" " + L("Navigation:", "Навигација:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Option", "Одабир опције") + " "),
                text(" [Enter] ") | bgcolor(Color::Green) | bold, text(" " + L("Trigger Optimization", "Примени") + " "),
                filler(),
                text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
            });
        } else if (current_tab == 3) {
            footer = hbox({
                text(" " + L("Navigation:", "Навигација:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                text(" [←/→] ") | bgcolor(Color::GrayDark), text(" " + L("Toggle Pane (Vault/AI)", "Промени фокус (Бележник/АИ)") + " "),
                text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Item", "Одабир ставке") + " "),
                text(" [Enter] ") | bgcolor(Color::Green) | bold, text(" " + L("Copy back / AI Recipe", "Копирај назад / АИ Рецепт") + " "),
                text(" [d] ") | bgcolor(Color::Red) | bold, text(" " + L("Delete Clip", "Обриши белешку") + " "),
                filler(),
                text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
            });
        }

        return vbox({ hbox({ sidebar | size(WIDTH, EQUAL, 28), right_panel | flex }), footer });
    });

    screen.Loop(renderer);

    refresh_ui_loop = false;
    if (data_thread.joinable()) data_thread.join();

    // Join all background threads (benchmarks, optimizations, AI queries)
    for (auto& t : bg_threads) {
        if (t.joinable()) t.join();
    }

    // Cleanup SQLite
    if (vault_db) {
        sqlite3_close(vault_db);
        vault_db = nullptr;
    }

    return 0;
}
