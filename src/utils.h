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

// Car-style rev gauge: 270° sweep with fixed color zones (ok / warn / redline),
// tick marks every 10%, and a needle pointing at the current value.
inline void draw_rev_gauge(ftxui::Canvas& c, int cx, int cy, int r, double pct,
                           ftxui::Color zone_ok, ftxui::Color zone_warn, ftxui::Color zone_red,
                           ftxui::Color needle_col, ftxui::Color dim_col) {
    const double a0 = -225.0 * M_PI / 180.0;
    const double sweep = 270.0 * M_PI / 180.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    double fill_t = pct / 100.0;

    // Zoned track: filled part bright + thickened, rest dimmed
    for (double t = 0.0; t <= 1.0; t += 0.006) {
        double a = a0 + t * sweep;
        ftxui::Color zone = (t < 0.6) ? zone_ok : (t < 0.8) ? zone_warn : zone_red;
        int x = static_cast<int>(cx + r * cos(a) * 1.5);
        int y = static_cast<int>(cy + r * sin(a));
        if (t <= fill_t) {
            c.DrawPoint(x, y, true, zone);
            int x2 = static_cast<int>(cx + (r - 1) * cos(a) * 1.5);
            int y2 = static_cast<int>(cy + (r - 1) * sin(a));
            c.DrawPoint(x2, y2, true, zone);
        } else {
            c.DrawPoint(x, y, true, dim_col);
        }
    }

    // Tick marks every 10%
    for (int k = 0; k <= 10; ++k) {
        double a = a0 + (k / 10.0) * sweep;
        ftxui::Color tc = (k < 6) ? zone_ok : (k < 8) ? zone_warn : zone_red;
        int x1 = static_cast<int>(cx + (r - 1) * cos(a) * 1.5);
        int y1 = static_cast<int>(cy + (r - 1) * sin(a));
        int x2 = static_cast<int>(cx + (r + 2) * cos(a) * 1.5);
        int y2 = static_cast<int>(cy + (r + 2) * sin(a));
        c.DrawPointLine(x1, y1, x2, y2, tc);
    }

    // Needle + hub
    double an = a0 + fill_t * sweep;
    int nx = static_cast<int>(cx + (r - 3) * cos(an) * 1.5);
    int ny = static_cast<int>(cy + (r - 3) * sin(an));
    c.DrawPointLine(cx, cy, nx, ny, needle_col);
    c.DrawPoint(cx + 1, cy, true, needle_col);
    c.DrawPoint(cx - 1, cy, true, needle_col);
}

// Letter-spaced header text ("CPU MATRIX" -> "C P U  M A T R I X"), UTF-8 aware.
inline std::string spaced(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char uc = static_cast<unsigned char>(s[i]);
        size_t len = (uc < 0x80) ? 1 : ((uc & 0xE0) == 0xC0) ? 2 : ((uc & 0xF0) == 0xE0) ? 3 : 4;
        if (s[i] == ' ') {
            out += "  ";
            i += 1;
            continue;
        }
        out += s.substr(i, len);
        out += ' ';
        i += len;
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Four-stop vertical gradient used for the fill under flux graph lines.
// top is drawn right under the line, base at the graph floor.
struct FluxPalette {
    ftxui::Color top;
    ftxui::Color mid1;
    ftxui::Color mid2;
    ftxui::Color base;
};

// Neon line graph: bloom halo + thin bright polyline + gradient fill under it.
// history values are scaled against scale_max into [0, h_px-4].
// pal == nullptr uses the signature AURA palette (pink -> amber -> red -> blue).
inline void draw_flux_graph(ftxui::Canvas& canvas, const std::vector<double>& history,
                            size_t max_size, double scale_max, int w_px, int h_px,
                            ftxui::Color line_col, ftxui::Color fill_col,
                            const FluxPalette* pal = nullptr) {
    if (history.size() < 2 || scale_max <= 0.0 || max_size < 2) return;
    (void)fill_col;
    static const FluxPalette signature = {
        ftxui::Color::RGB(255, 45, 130), ftxui::Color::RGB(255, 210, 0),
        ftxui::Color::RGB(255, 70, 70), ftxui::Color::RGB(15, 60, 150)
    };
    const FluxPalette& p = pal ? *pal : signature;
    auto flux_grad = [&p](float t) {
        if (t < 0.35f) return ftxui::Color::Interpolate(t / 0.35f, p.top, p.mid1);
        if (t < 0.75f) return ftxui::Color::Interpolate((t - 0.35f) / 0.40f, p.mid1, p.mid2);
        return ftxui::Color::Interpolate((t - 0.75f) / 0.25f, p.mid2, p.base);
    };
    ftxui::Color halo = ftxui::Color::Interpolate(0.35f, ftxui::Color::Black, line_col);
    int max_y = h_px - 4;
    int base_y = h_px - 2;
    int lx = -1, ly = -1;
    for (size_t i = 0; i < history.size(); ++i) {
        int x = static_cast<int>((static_cast<double>(i) / (max_size - 1)) * (w_px - 1));
        double v = history[i] / scale_max;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        int y = base_y - static_cast<int>(v * max_y);
        if (lx != -1) {
            canvas.DrawPointLine(lx, ly - 1, x, y - 1, halo);
            canvas.DrawPointLine(lx, ly, x, y, line_col);
            for (int fx = lx; fx <= x; ++fx) {
                int top = ly + (x == lx ? 0 : ((fx - lx) * (y - ly)) / (x - lx));
                int span = base_y - top;
                for (int fy = top + 1; fy <= base_y; ++fy) {
                    float depth = span > 0 ? static_cast<float>(fy - top) / span : 1.0f;
                    canvas.DrawPoint(fx, fy, true, flux_grad(depth));
                }
            }
        }
        lx = x; ly = y;
    }
}

// Translation helper: returns Serbian Cyrillic when theme is SRBIJA, English otherwise.
// Usage: L("English text", "Српски текст")
inline std::string L(const std::string& en, const std::string& sr, Theme current_theme) {
    return (current_theme == SRBIJA) ? sr : en;
}
