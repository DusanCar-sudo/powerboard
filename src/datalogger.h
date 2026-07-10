#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>

class DataLogger {
public:
    explicit DataLogger(std::string base_dir = ".")
        : base_dir_(std::move(base_dir)) {}

    ~DataLogger() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (file_.is_open()) file_.close();
    }

    void log(double power_w, double cumulative_kwh, double cost_usd) {
        std::lock_guard<std::mutex> lk(mutex_);

        auto now     = std::chrono::system_clock::now();
        auto tt      = std::chrono::system_clock::to_time_t(now);
        auto* tm     = std::localtime(&tt);
        int  year    = tm->tm_year + 1900;
        int  month   = tm->tm_mon + 1;

        if (year != current_year_ || month != current_month_) {
            if (file_.is_open()) file_.close();

            std::ostringstream fname;
            fname << base_dir_ << "/powerboard_"
                  << year << "-" << std::setw(2) << std::setfill('0') << month
                  << ".csv";
            file_.open(fname.str(), std::ios::app);
            current_year_  = year;
            current_month_ = month;

            // Write header if file is new (empty)
            file_.seekp(0, std::ios::end);
            if (file_.tellp() == 0) {
                file_ << "timestamp,power_w,cumulative_kwh,cost_usd\n";
            }
        }

        ensure_open();
        if (!file_.is_open()) return;

        file_ << std::put_time(tm, "%Y-%m-%dT%H:%M:%S") << ","
              << power_w << ","
              << cumulative_kwh << ","
              << cost_usd << "\n";
        file_.flush();
    }

private:
    void ensure_open() {
        if (file_.is_open()) return;

        auto now   = std::chrono::system_clock::now();
        auto tt    = std::chrono::system_clock::to_time_t(now);
        auto* tm   = std::localtime(&tt);
        current_year_  = tm->tm_year + 1900;
        current_month_ = tm->tm_mon + 1;

        std::ostringstream fname;
        fname << base_dir_ << "/powerboard_"
              << current_year_ << "-" << std::setw(2) << std::setfill('0') << current_month_
              << ".csv";
        file_.open(fname.str(), std::ios::app);
        if (file_.tellp() == 0) {
            file_ << "timestamp,power_w,cumulative_kwh,cost_usd\n";
        }
    }

    std::string   base_dir_;
    std::ofstream file_;
    std::mutex    mutex_;
    int           current_month_ = -1;
    int           current_year_  = -1;
};

inline void log_to_csv(DataLogger& logger, double power_w, double cumulative_kwh, double cost_usd) {
    logger.log(power_w, cumulative_kwh, cost_usd);
}
