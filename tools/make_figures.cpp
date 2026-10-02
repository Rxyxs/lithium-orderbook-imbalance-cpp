// Genera las figuras del README como SVG, escritas a mano con la libreria
// estandar.
//
// El proyecto declara cero dependencias externas y ninguna capa de build
// adicional; traer matplotlib o una libreria de graficos contradiria
// exactamente la tesis que el README defiende. Un SVG es texto, y un grafico
// de lineas y barras es geometria simple, asi que se emite directo.
//
// Uso:
//   make_figures.exe <results.csv> [--symbol SQM] [--outdir docs/figures]
//
// Lee la salida del motor (la misma que produce loi_engine.exe) y vuelve a
// correr ambos estimadores sobre la serie de net_imbalance, de modo que las
// cifras dibujadas salen de los mismos headers que usa el motor en vivo.

#include "ewma_zscore.hpp"
#include "robust_stats.hpp"
#include "robust_zscore.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Ventana del shock inyectado por tools/generate_sample_data.cpp.
constexpr long long kShockStartMs = 4'320'000;
constexpr long long kShockEndMs = 4'325'000;

constexpr double kAlertSigma = 3.0;
constexpr double kDof = 4.0;

struct Window {
    long long start_ms = 0;
    double net_imbalance = 0.0;
};

struct Series {
    std::vector<Window> windows;
    std::vector<std::optional<double>> gaussian_z;
    std::vector<std::optional<double>> robust_t;
};

// ---------------------------------------------------------------------------
// SVG minimo
// ---------------------------------------------------------------------------
class Svg {
public:
    Svg(int width, int height) : width_(width), height_(height) {
        out_ << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width
             << "\" height=\"" << height << "\" viewBox=\"0 0 " << width << " "
             << height << "\" font-family=\"Segoe UI, Helvetica, Arial, sans-serif\">\n";
        rect(0, 0, width, height, "#FFFFFF", "", 0);
    }

    void rect(double x, double y, double w, double h, const std::string& fill,
              const std::string& stroke = "", double stroke_w = 1.0, double opacity = 1.0) {
        out_ << "  <rect x=\"" << f(x) << "\" y=\"" << f(y) << "\" width=\"" << f(w)
             << "\" height=\"" << f(h) << "\" fill=\"" << fill << "\"";
        if (!stroke.empty()) out_ << " stroke=\"" << stroke << "\" stroke-width=\"" << f(stroke_w) << "\"";
        if (opacity < 1.0) out_ << " fill-opacity=\"" << f(opacity) << "\"";
        out_ << "/>\n";
    }

    void line(double x1, double y1, double x2, double y2, const std::string& stroke,
              double w = 1.0, const std::string& dash = "") {
        out_ << "  <line x1=\"" << f(x1) << "\" y1=\"" << f(y1) << "\" x2=\"" << f(x2)
             << "\" y2=\"" << f(y2) << "\" stroke=\"" << stroke << "\" stroke-width=\"" << f(w) << "\"";
        if (!dash.empty()) out_ << " stroke-dasharray=\"" << dash << "\"";
        out_ << "/>\n";
    }

    void polyline(const std::vector<std::pair<double, double>>& pts, const std::string& stroke,
                  double w = 1.0, double opacity = 1.0) {
        if (pts.empty()) return;
        out_ << "  <polyline fill=\"none\" stroke=\"" << stroke << "\" stroke-width=\"" << f(w)
             << "\" stroke-opacity=\"" << f(opacity) << "\" points=\"";
        for (const auto& [x, y] : pts) out_ << f(x) << "," << f(y) << " ";
        out_ << "\"/>\n";
    }

    void circle(double cx, double cy, double r, const std::string& fill, double opacity = 1.0) {
        out_ << "  <circle cx=\"" << f(cx) << "\" cy=\"" << f(cy) << "\" r=\"" << f(r)
             << "\" fill=\"" << fill << "\" fill-opacity=\"" << f(opacity) << "\"/>\n";
    }

    void text(double x, double y, const std::string& s, double size = 12,
              const std::string& fill = "#2B2B2B", const std::string& anchor = "start",
              bool bold = false) {
        out_ << "  <text x=\"" << f(x) << "\" y=\"" << f(y) << "\" font-size=\"" << f(size)
             << "\" fill=\"" << fill << "\" text-anchor=\"" << anchor << "\"";
        if (bold) out_ << " font-weight=\"600\"";
        out_ << ">" << escape(s) << "</text>\n";
    }

    bool save(const std::string& path) {
        out_ << "</svg>\n";
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        file << out_.str();
        return true;
    }

private:
    static std::string escape(const std::string& s) {
        std::string r;
        for (char c : s) {
            switch (c) {
                case '&': r += "&amp;"; break;
                case '<': r += "&lt;"; break;
                case '>': r += "&gt;"; break;
                default: r += c;
            }
        }
        return r;
    }

    static std::string f(double v) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(2) << v;
        auto s = o.str();
        // Recorta ceros finales para no inflar el archivo.
        if (s.find('.') != std::string::npos) {
            s.erase(s.find_last_not_of('0') + 1);
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        return s;
    }

    int width_, height_;
    std::ostringstream out_;
};

std::string fmt(double v, int decimals) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(decimals) << v;
    return o.str();
}

std::string with_thousands(long long v) {
    auto s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

// ---------------------------------------------------------------------------
// Carga y recomputo
// ---------------------------------------------------------------------------
std::optional<Series> load(const std::string& path, const std::string& symbol) {
    std::ifstream in(path);
    if (!in) return std::nullopt;

    std::string line;
    if (!std::getline(in, line)) return std::nullopt;  // cabecera

    Series s;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string start_ms, sym, buy, sell, net;
        if (!std::getline(ss, start_ms, ',')) continue;
        if (!std::getline(ss, sym, ',')) continue;
        if (sym != symbol) continue;
        if (!std::getline(ss, buy, ',')) continue;
        if (!std::getline(ss, sell, ',')) continue;
        if (!std::getline(ss, net, ',')) continue;
        try {
            s.windows.push_back({std::stoll(start_ms), std::stod(net)});
        } catch (...) {
            continue;
        }
    }
    if (s.windows.empty()) return std::nullopt;

    // Mismos parametros que main.cpp y que el test de comparacion.
    loi::EwmaZScore ewma(/*alpha=*/0.05, /*warmup_samples=*/30, /*min_std=*/1e-6);
    loi::RobustZScore robust(/*window_size=*/60, /*warmup_samples=*/30, /*min_mad=*/1e-6);
    for (const auto& w : s.windows) {
        s.gaussian_z.push_back(ewma.update(w.net_imbalance));
        s.robust_t.push_back(robust.update(w.net_imbalance));
    }
    return s;
}

// Simbolos presentes en el archivo de resultados, en orden de aparicion.
std::vector<std::string> symbols_in(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> out;
    std::string line;
    if (!in || !std::getline(in, line)) return out;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string start_ms, sym;
        if (!std::getline(ss, start_ms, ',')) continue;
        if (!std::getline(ss, sym, ',')) continue;
        if (std::find(out.begin(), out.end(), sym) == out.end()) out.push_back(sym);
    }
    return out;
}

bool in_shock(long long t) { return t >= kShockStartMs && t < kShockEndMs; }

struct Counts {
    std::size_t inside = 0;
    std::size_t outside = 0;
    std::size_t total() const { return inside + outside; }
};

Counts count_alerts(const Series& s, const std::vector<std::optional<double>>& stat, double threshold) {
    Counts c;
    for (std::size_t i = 0; i < stat.size(); ++i) {
        if (!stat[i].has_value() || std::fabs(*stat[i]) < threshold) continue;
        if (in_shock(s.windows[i].start_ms)) ++c.inside;
        else ++c.outside;
    }
    return c;
}

// ---------------------------------------------------------------------------
// Figura 1: las dos estadisticas en el tiempo
// ---------------------------------------------------------------------------
void figure_timeline(const Series& s, double t_crit, const std::string& symbol,
                     const std::string& path) {
    constexpr int W = 1100, H = 560;
    constexpr double L = 78, R = 28, T = 86, B = 64;
    const double pw = W - L - R, ph = H - T - B;

    Svg svg(W, H);

    // Escala comun para que las dos series sean comparables de un vistazo.
    double max_abs = 0;
    for (const auto& v : s.gaussian_z) if (v) max_abs = std::max(max_abs, std::fabs(*v));
    for (const auto& v : s.robust_t) if (v) max_abs = std::max(max_abs, std::fabs(*v));
    const double y_max = std::ceil(max_abs / 10.0) * 10.0;

    const double t0 = static_cast<double>(s.windows.front().start_ms);
    const double t1 = static_cast<double>(s.windows.back().start_ms);
    auto sx = [&](double t) { return L + (t - t0) / (t1 - t0) * pw; };
    auto sy = [&](double v) { return T + ph - (v / y_max) * ph; };

    // Banda del shock inyectado.
    svg.rect(sx(kShockStartMs), T, std::max(2.0, sx(kShockEndMs) - sx(kShockStartMs)), ph,
             "#B5553D", "", 0, 0.14);

    // Grilla horizontal.
    for (int i = 0; i <= 5; ++i) {
        const double v = y_max * i / 5.0;
        svg.line(L, sy(v), L + pw, sy(v), "#D9D9D9", 0.8);
        svg.text(L - 8, sy(v) + 4, fmt(v, 0), 11, "#6A6A6A", "end");
    }

    auto draw = [&](const std::vector<std::optional<double>>& stat, const std::string& color,
                    double width, double opacity) {
        std::vector<std::pair<double, double>> pts;
        for (std::size_t i = 0; i < stat.size(); ++i) {
            if (!stat[i]) continue;
            pts.emplace_back(sx(static_cast<double>(s.windows[i].start_ms)),
                             sy(std::min(std::fabs(*stat[i]), y_max)));
        }
        svg.polyline(pts, color, width, opacity);
    };

    draw(s.gaussian_z, "#6E8CA0", 0.8, 0.85);
    draw(s.robust_t, "#B5553D", 0.9, 0.9);

    // Umbrales.
    svg.line(L, sy(kAlertSigma), L + pw, sy(kAlertSigma), "#6E8CA0", 1.6, "6,4");
    svg.text(L + pw - 6, sy(kAlertSigma) - 7, "Gaussian threshold  |z| = " + fmt(kAlertSigma, 1),
             11, "#6E8CA0", "end");
    svg.line(L, sy(t_crit), L + pw, sy(t_crit), "#B5553D", 1.6, "6,4");
    svg.text(L + pw - 6, sy(t_crit) - 7,
             "Robust threshold  |t| = " + fmt(t_crit, 4) + "  (Student-t, dof " + fmt(kDof, 0) + ")",
             11, "#B5553D", "end");

    // Ejes.
    svg.line(L, T + ph, L + pw, T + ph, "#B9B9B9", 1.2);
    svg.line(L, T, L, T + ph, "#B9B9B9", 1.2);
    for (int i = 0; i <= 6; ++i) {
        const double t = t0 + (t1 - t0) * i / 6.0;
        svg.line(sx(t), T + ph, sx(t), T + ph + 5, "#B9B9B9", 1.0);
        svg.text(sx(t), T + ph + 20, fmt(t / 60000.0, 0), 11, "#6A6A6A", "middle");
    }
    svg.text(L + pw / 2, H - 16, "Minutes into the session", 12, "#2B2B2B", "middle");
    svg.text(16, T - 16, "|statistic|", 12, "#2B2B2B");

    svg.text(L, 30, "Both detectors on the same " + symbol + " order-flow series", 16, "#2B2B2B",
             "start", true);
    svg.text(L, 52,
             "The Gaussian EWMA (blue) crosses its own line constantly on fat-tailed noise. "
             "The robust statistic (red) stays flat until the shock.",
             12, "#6A6A6A");
    svg.text(sx((kShockStartMs + kShockEndMs) / 2.0), T - 6, "injected shock", 11, "#B5553D",
             "middle", true);

    if (!svg.save(path)) {
        std::fprintf(stderr, "No se pudo escribir %s\n", path.c_str());
        return;
    }
    std::printf("  escrito %s\n", path.c_str());
}

// ---------------------------------------------------------------------------
// Figura 2: alertas dentro y fuera del shock
// ---------------------------------------------------------------------------
void figure_alert_counts(const Counts& gauss, const Counts& robust, std::size_t n_windows,
                         std::size_t shock_windows, const std::string& path) {
    constexpr int W = 940, H = 520;
    constexpr double T = 104, B = 96;
    const double ph = H - T - B;

    Svg svg(W, H);

    // Dos paneles con escalas propias. Con una sola escala, los 294 falsos
    // positivos del gaussiano aplastan el otro hecho -- que captura 2 de las 10
    // ventanas del shock -- hasta volverlo invisible: dos pixeles de alto.
    // Son dos preguntas distintas y merecen dos ejes distintos.
    struct Panel {
        std::string title;
        std::string subtitle;
        double left, width, y_max;
        double values[2];
        const char* colors[2];
        bool lower_is_better;
    };

    const double pw = (W - 92 - 36 - 56) / 2.0;
    const std::vector<Panel> panels = {
        {"False alarms", "windows flagged outside the shock",
         92, pw, std::max(1.0, static_cast<double>(gauss.outside) * 1.3),
         {static_cast<double>(gauss.outside), static_cast<double>(robust.outside)},
         {"#6E8CA0", "#B5553D"}, true},
        {"Shock windows caught",
         "out of " + std::to_string(shock_windows) + " true shock windows",
         92 + pw + 56, pw, static_cast<double>(shock_windows) * 1.3,
         {static_cast<double>(gauss.inside), static_cast<double>(robust.inside)},
         {"#6E8CA0", "#B5553D"}, false},
    };

    const char* flat_names[2] = {"Gaussian EWMA", "MAD + Student-t"};

    for (const auto& p : panels) {
        auto sy = [&](double v) { return T + ph - (v / p.y_max) * ph; };

        for (int i = 0; i <= 4; ++i) {
            const double v = p.y_max * i / 4.0;
            svg.line(p.left, sy(v), p.left + p.width, sy(v), "#D9D9D9", 0.8);
            svg.text(p.left - 8, sy(v) + 4, fmt(v, 0), 11, "#6A6A6A", "end");
        }

        const double bw = p.width / 3.2;
        for (int i = 0; i < 2; ++i) {
            const double cx = p.left + p.width * (i + 0.5) / 2.0;
            const double h = (p.values[i] / p.y_max) * ph;
            svg.rect(cx - bw / 2.0, T + ph - h, bw, std::max(h, 1.5), p.colors[i], "#FFFFFF", 1.2);
            svg.text(cx, T + ph - std::max(h, 1.5) - 9, fmt(p.values[i], 0), 15, p.colors[i],
                     "middle", true);
            svg.text(cx, T + ph + 22, flat_names[i], 12, "#2B2B2B", "middle", true);
        }

        svg.line(p.left, T + ph, p.left + p.width, T + ph, "#B9B9B9", 1.2);
        svg.line(p.left, T, p.left, T + ph, "#B9B9B9", 1.2);
        svg.text(p.left, T - 30, p.title, 14, "#2B2B2B", "start", true);
        svg.text(p.left, T - 12, p.subtitle, 11, "#6A6A6A");
        svg.text(p.left + p.width / 2, T + ph + 46,
                 p.lower_is_better ? "lower is better" : "higher is better", 11, "#8A8A8A", "middle");
    }

    svg.text(92, 34, "The robust detector wins on both counts, not just on false alarms", 16,
             "#2B2B2B", "start", true);
    svg.text(92, 56,
             "Same " + with_thousands(static_cast<long long>(n_windows)) +
                 " windows, same data, both calibrated to the same \"as rare as a Gaussian "
                 "3-sigma event\" target.",
             12, "#6A6A6A");
    svg.text(92, 76,
             "The Gaussian EWMA raises " + with_thousands(static_cast<long long>(gauss.outside)) +
                 " false alarms and still misses most of the shock: its own mean and variance "
                 "inflate during a sustained burst,",
             12, "#2B2B2B");
    svg.text(92, 92, "so after the first windows the shock starts to look normal to it.", 12,
             "#2B2B2B");

    if (!svg.save(path)) {
        std::fprintf(stderr, "No se pudo escribir %s\n", path.c_str());
        return;
    }
    std::printf("  escrito %s\n", path.c_str());
}

// ---------------------------------------------------------------------------
// Figura 3: por que el umbral se mueve con los grados de libertad
// ---------------------------------------------------------------------------
void figure_critical_values(const std::string& path) {
    constexpr int W = 880, H = 500;
    constexpr double L = 84, R = 150, T = 88, B = 70;
    const double pw = W - L - R, ph = H - T - B;

    Svg svg(W, H);

    const double alpha = loi::gaussian_two_sided_pvalue(kAlertSigma);
    std::vector<std::pair<double, double>> curve;
    double y_max = 0;
    for (int dof = 3; dof <= 40; ++dof) {
        const double cv = loi::student_t_critical_value(alpha, static_cast<double>(dof));
        curve.emplace_back(static_cast<double>(dof), cv);
        y_max = std::max(y_max, cv);
    }
    y_max = std::ceil(y_max) + 0.5;

    auto sx = [&](double dof) { return L + (dof - 3.0) / (40.0 - 3.0) * pw; };
    auto sy = [&](double v) { return T + ph - (v / y_max) * ph; };

    for (int i = 0; i <= 5; ++i) {
        const double v = y_max * i / 5.0;
        svg.line(L, sy(v), L + pw, sy(v), "#D9D9D9", 0.8);
        svg.text(L - 10, sy(v) + 4, fmt(v, 1), 11, "#6A6A6A", "end");
    }

    std::vector<std::pair<double, double>> pts;
    for (const auto& [dof, cv] : curve) pts.emplace_back(sx(dof), sy(cv));
    svg.polyline(pts, "#B5553D", 2.2);

    // Asintota gaussiana.
    svg.line(L, sy(kAlertSigma), L + pw, sy(kAlertSigma), "#6E8CA0", 1.6, "6,4");
    svg.text(L + pw + 8, sy(kAlertSigma) + 4, "Gaussian limit 3.00", 11, "#6E8CA0");

    const double cv4 = loi::student_t_critical_value(alpha, 4.0);
    svg.circle(sx(4), sy(cv4), 6, "#B5553D");
    svg.text(sx(4) + 12, sy(cv4) - 8, "dof 4 (this project): " + fmt(cv4, 4), 12, "#B5553D",
             "start", true);

    svg.line(L, T + ph, L + pw, T + ph, "#B9B9B9", 1.2);
    svg.line(L, T, L, T + ph, "#B9B9B9", 1.2);
    for (int dof : {3, 5, 10, 15, 20, 30, 40}) {
        svg.line(sx(dof), T + ph, sx(dof), T + ph + 5, "#B9B9B9", 1.0);
        svg.text(sx(dof), T + ph + 20, std::to_string(dof), 11, "#6A6A6A", "middle");
    }
    svg.text(L + pw / 2, H - 14, "Degrees of freedom assumed for the tail", 12, "#2B2B2B", "middle");
    svg.text(16, T - 14, "critical value", 12, "#2B2B2B");

    svg.text(L, 32, "The same alert rate costs a much higher threshold on a fat tail", 16,
             "#2B2B2B", "start", true);
    svg.text(L, 54,
             "Critical value for a two-sided p of " + fmt(alpha, 5) +
                 " -- the probability a Gaussian 3-sigma rule accepts.",
             12, "#6A6A6A");
    svg.text(L, 74,
             "Assuming dof 4 instead of normality more than doubles the bar a window has to clear.",
             12, "#2B2B2B");

    if (!svg.save(path)) {
        std::fprintf(stderr, "No se pudo escribir %s\n", path.c_str());
        return;
    }
    std::printf("  escrito %s\n", path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    std::string results = "data/sample_results.csv";
    std::string symbol = "SQM";
    std::string outdir = "docs/figures";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--symbol" && i + 1 < argc) symbol = argv[++i];
        else if (a == "--outdir" && i + 1 < argc) outdir = argv[++i];
        else if (a.rfind("--", 0) != 0) results = a;
    }

    auto series = load(results, symbol);
    if (!series) {
        std::fprintf(stderr,
                     "No se pudo leer %s (o no hay filas para el simbolo %s).\n"
                     "Genera los resultados primero:\n"
                     "  .\\bin\\loi_engine.exe data\\lithium_ticks_sample.csv --out data\\sample_results.csv\n",
                     results.c_str(), symbol.c_str());
        return 1;
    }

    const double alpha = loi::gaussian_two_sided_pvalue(kAlertSigma);
    const double t_crit = loi::student_t_critical_value(alpha, kDof);

    // La linea de tiempo usa un solo simbolo (el shock esta inyectado en uno),
    // pero los conteos agregan todos: la cifra que el README publica es la del
    // dataset completo, y una figura que mostrara solo un simbolo contra ese
    // texto obligaria al lector a reconciliar dos numeros distintos.
    Counts gauss_all, robust_all;
    std::size_t windows_all = 0;
    for (const std::string& sym : symbols_in(results)) {
        auto s = load(results, sym);
        if (!s) continue;
        const Counts g = count_alerts(*s, s->gaussian_z, kAlertSigma);
        const Counts r = count_alerts(*s, s->robust_t, t_crit);
        gauss_all.inside += g.inside;
        gauss_all.outside += g.outside;
        robust_all.inside += r.inside;
        robust_all.outside += r.outside;
        windows_all += s->windows.size();
        std::printf("  %-4s ventanas=%-7s gaussiano=%-4zu (%zu en shock)  robusto=%-3zu (%zu en shock)\n",
                    sym.c_str(), with_thousands(static_cast<long long>(s->windows.size())).c_str(),
                    g.total(), g.inside, r.total(), r.inside);
    }

    std::printf("\nTotal sobre %s ventanas:\n",
                with_thousands(static_cast<long long>(windows_all)).c_str());
    std::printf("  gaussiano |z|>=%.1f   -> %zu alertas (%zu dentro del shock, %zu falsas)\n",
                kAlertSigma, gauss_all.total(), gauss_all.inside, gauss_all.outside);
    std::printf("  robusto   |t|>=%.4f -> %zu alertas (%zu dentro del shock, %zu falsas)\n",
                t_crit, robust_all.total(), robust_all.inside, robust_all.outside);

    figure_timeline(*series, t_crit, symbol, outdir + "/detector_timeline.svg");
    const std::size_t shock_windows =
        static_cast<std::size_t>((kShockEndMs - kShockStartMs) / 500);
    figure_alert_counts(gauss_all, robust_all, windows_all, shock_windows,
                        outdir + "/alert_counts.svg");
    figure_critical_values(outdir + "/student_t_critical_values.svg");

    return 0;
}
