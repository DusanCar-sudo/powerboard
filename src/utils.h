#pragma once
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include "types.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/canvas.hpp"

inline std::string format_double(double val, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << val;
    return oss.str();
}

inline std::string escape_json_string(const std::string& s) {
    std::ostringstream o;
    for (auto c : s) {
        if (c == '"') o << "\\\"";
        else if (c == '\\') o << "\\\\";
        else if (c == '\b') o << "\\b";
        else if (c == '\f') o << "\\f";
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c == '\t') o << "\\t";
        else if (c < 0x20) {
            o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
        } else {
            o << c;
        }
    }
    return o.str();
}

inline void draw_arc(ftxui::Canvas& canvas, int cx, int cy, int r, double percentage, ftxui::Color col) {
    double start_angle = -225.0 * (M_PI / 180.0);
    double total_angle = 270.0 * (M_PI / 180.0) * (percentage / 100.0);
    double end_angle = start_angle + total_angle;

    for (double a = start_angle; a <= end_angle; a += 0.03) {
        int x = static_cast<int>(cx + r * cos(a) * 1.5);
        int y = static_cast<int>(cy + r * sin(a));
        canvas.DrawPoint(x, y, true, col);
    }

    double max_end_angle = start_angle + 270.0 * (M_PI / 180.0);
    for (double a = end_angle; a <= max_end_angle; a += 0.03) {
        int x = static_cast<int>(cx + r * cos(a) * 1.5);
        int y = static_cast<int>(cy + r * sin(a));
        canvas.DrawPoint(x, y, true, ftxui::Color::GrayDark);
    }
}

// Translation helper: returns Serbian Cyrillic when theme is SRBIJA, English otherwise.
// Usage: L("English text", "Српски текст")
inline std::string L(const std::string& en, const std::string& sr, Theme current_theme) {
    return (current_theme == SRBIJA) ? sr : en;
}
