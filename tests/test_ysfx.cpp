// Integration tests for the actual JSFX, using the ysfx library.
#include "ysfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static void near(double actual, double expected, double tolerance, const char *message)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL: %s: actual %.12g, expected %.12g\n", message, actual, expected);
        std::exit(1);
    }
}

struct Effect {
    ysfx_config_t *config = ysfx_config_new();
    ysfx_t *fx = ysfx_new(config);

    Effect()
    {
        check(ysfx_load_file(fx, "jsfx/AZMbeziExp.jsfx", 0), "load JSFX");
        check(ysfx_compile(fx, 0), "compile JSFX including GUI");
        ysfx_set_sample_rate(fx, 48000);
        ysfx_set_block_size(fx, 1);
        ysfx_init(fx);
    }
    ~Effect() { ysfx_free(fx); ysfx_config_free(config); }
    void set(int slider, double value) { ysfx_slider_set_value(fx, slider - 1, value); }
    double get(int slider) { return ysfx_slider_get_value(fx, slider - 1); }
    double var(const char *name)
    {
        auto *p = ysfx_find_var(fx, name);
        check(p != nullptr, name);
        return *p;
    }
    // GUI 座標の変数はスーパーサンプリング後の座標なので、マウス座標へ戻す
    double px(const char *name) { return var(name) / var("SS"); }
    void set_var(const char *name, double value)
    {
        auto *p = ysfx_find_var(fx, name);
        check(p != nullptr, name);
        *p = value;
    }
    double run(double left, double right)
    {
        const double *in[] = {&left, &right};
        double l = 0, r = 0;
        double *out[] = {&l, &r};
        ysfx_process_double(fx, in, out, 2, 2, 1);
        if (right != 0 && left != 0)
            near(l / left, r / right, 1e-10, "stereo-linked gain");
        return l;
    }
};

// Curve strength in -1..1: the control point sits at (0.5 + c/2, 0.5 - c/2).
static const double kAttackCurve = 0.5;
static const double kReleaseCurve = -0.5;

static double curve(double level, double close, double open, double c, double range = 40)
{
    double t = (level - close) / (open - close);
    if (t <= 0) return -range;
    if (t >= 1) return 0;
    double x1 = 0.5 + 0.5 * c, y1 = 0.5 - 0.5 * c;
    auto q = [](double u, double p1) { return u * (2 * (1 - u) * p1 + u); };
    double lo = 0, hi = 1;
    for (int i = 0; i < 100; ++i) {
        double m = (lo + hi) / 2;
        (q(m, x1) < t ? lo : hi) = m;
    }
    double u = (lo + hi) / 2;
    return -range * (1 - q(u, y1));
}

static void compare_reference(double attack_mix, double release_mix, bool sync)
{
    Effect e;
    e.set(15, attack_mix * 100);
    e.set(16, release_mix * 100);
    e.set(5, sync ? 1 : 0);
    double msq = 0, gain = 0;
    double dir_a_f = 0, dir_r_f = 0, prev_dir_a = 0, prev_dir_r = 0;
    bool up = false;
    const double rms_k = 1 - std::exp(-1.0 / (0.040 * 48000));
    const double dir_k = 1 - std::exp(-1.0 / (0.002 * 48000));
    const double open_k = 1 - std::exp(-1.0 / (0.010 * 48000));
    const double close_k = 1 - std::exp(-1.0 / (0.120 * 48000));
    const double rms_gain = std::pow(10.0, 6.0 / 20);
    for (int i = 0; i < 16000; ++i) {
        double envelope = 0.0003 + 0.003 * (1 + std::sin(i * 0.0013));
        double left = envelope * std::sin(i * 0.13);
        double right = envelope * 0.7 * std::cos(i * 0.11);
        double a = std::max(std::abs(left), std::abs(right));
        msq = std::max(msq + rms_k * (a * a - msq), 1e-24);
        double rms = std::sqrt(2 * msq) * rms_gain;
        auto mix = [&](double peak, double r, double ratio) {
            return std::max(peak * (1 - ratio) + r * ratio, 1e-12);
        };
        double av = mix(a, rms, attack_mix), rv = mix(a, rms, release_mix);
        dir_a_f += (av - dir_a_f) * dir_k;
        dir_r_f += (rv - dir_r_f) * dir_k;
        if (i != 0) {
            if (dir_a_f > prev_dir_a) up = true;
            else if (dir_r_f < prev_dir_r) up = false;
        }
        prev_dir_a = dir_a_f;
        prev_dir_r = dir_r_f;
        double level = 20 * std::log10(up ? av : rv);
        double target = up || sync ? curve(level, -65, -55, kAttackCurve)
                                   : curve(level, -55, -40, kReleaseCurve);
        if (i == 0) gain = target;
        else if (gain < target) gain = std::min(target, gain + (target - gain + 1e-9) * open_k);
        else if (gain > target) gain = std::max(target, gain + (target - gain - 1e-9) * close_k);
        double output = e.run(left, right);
        near(e.var("gdb"), gain, 1e-7, "reference gain");
        near(e.var("dir_up"), up ? 1 : 0, 0, "direction");
        near(output, left * std::pow(10.0, gain / 20), 1e-10, "output gain");
    }
}

static void test_direction_and_automation()
{
    Effect e;
    e.set(15, 0); e.set(16, 100);
    e.run(0.001, 0.001);
    e.set_var("gain_init", 0);
    e.set_var("dir_a_f", 0.5);
    e.set_var("prev_dir_a", 0.1);
    e.set_var("dir_r_f", 0.1);
    e.set_var("prev_dir_r", 0.5);
    e.run(0, 0);
    near(e.var("dir_up"), 1, 0, "attack wins simultaneous directions");
    e.set_var("dir_up", 1);
    e.set_var("dir_a_f", 0.1);
    e.set_var("prev_dir_a", 0.5);
    e.set_var("dir_r_f", 0.5);
    e.set_var("prev_dir_r", 0.1);
    e.run(0, 0);
    near(e.var("dir_up"), 1, 0, "neither direction keeps previous choice");
}

static void test_detector_and_gain()
{
    Effect e;
    e.set(15, 0); e.set(16, 0);
    e.run(0.1, 0.1);
    double previous_msq = e.var("msq");
    e.run(0, 0);
    near(e.var("pk_lin"), 0, 0, "peak has no release envelope");
    double rms_k = 1 - std::exp(-1.0 / (0.040 * 48000));
    near(e.var("msq"), previous_msq * (1 - rms_k), 1e-14, "RMS has only window decay");

    Effect moving;
    moving.run(0.001, 0.001);
    moving.set_var("gdb", -25);
    moving.set_var("dir_up", 0);
    moving.set_var("dir_a_f", 0.1);
    moving.set_var("prev_dir_a", 0.5);
    moving.set_var("dir_r_f", 0.5);
    moving.set_var("prev_dir_r", 0.1);
    moving.set_var("gain_init", 0);
    moving.run(0.001, 0.001);
    near(moving.var("dir_up"), 0, 0, "neither direction preserves release");
    check(moving.var("gdb") < -25, "preserved direction does not hold gain");
}

static void test_block_sizes()
{
    for (double rate : {22050.0, 48000.0, 96000.0, 192000.0}) {
        Effect single, block;
        ysfx_set_sample_rate(single.fx, rate);
        ysfx_set_sample_rate(block.fx, rate);
        ysfx_set_block_size(block.fx, 2048);
        ysfx_init(single.fx);
        ysfx_init(block.fx);
        std::vector<double> left(2048), right(2048), out_l(2048), out_r(2048);
        for (size_t i = 0; i < left.size(); ++i) {
            left[i] = 0.001 * std::sin(i * 0.07);
            right[i] = 0.0007 * std::cos(i * 0.09);
        }
        const double *ins[] = {left.data(), right.data()};
        double *outs[] = {out_l.data(), out_r.data()};
        ysfx_process_double(block.fx, ins, outs, 2, 2, left.size());
        for (size_t i = 0; i < left.size(); ++i) {
            double out = single.run(left[i], right[i]);
            near(out, out_l[i], 1e-12, "block-size independent");
            check(std::abs(out_l[i]) <= std::abs(left[i]) + 1e-12, "no gain boost");
        }
    }
}

static void test_bypass()
{
    Effect e;
    double l = 0;
    for (int i = 0; i < 2000; ++i) l = e.run(0.001, 0.001);
    check(std::abs(l) < 0.001 * 0.5, "expander attenuates quiet input");
    e.set(17, 1);
    for (int i = 0; i < 4800; ++i) l = e.run(0.001, 0.001);
    near(l, 0.001, 1e-9, "bypass passes input through");
    e.set(17, 0);
    for (int i = 0; i < 4800; ++i) l = e.run(0.001, 0.001);
    check(std::abs(l) < 0.001 * 0.5, "bypass off restores processing");
}

static void test_clamping()
{
    Effect e;
    e.set(5, 1);
    e.set(13, -50);
    e.run(0.001, 0.001);
    near(e.get(2), -50, 1e-7, "attack close clamped");
    near(e.get(3), -49.9, 1e-7, "attack open separated");
    near(e.get(6), -50, 1e-7, "synced release close clamped");
    near(e.get(7), -40, 1e-7, "synced release open stays inside the viewport");
    e.set(13, -90);
    e.run(0.001, 0.001);
    near(e.get(2), -50, 1e-7, "widening does not restore values");
    e.set(2, -80);
    e.set(3, -75);
    e.run(0.001, 0.001);
    near(e.get(2), -80, 1e-7, "automation remains within viewport");
    e.set(14, -60);
    e.set(13, -30);
    e.run(0.001, 0.001);
    check(e.get(14) - e.get(13) >= 10 - 1e-8, "automated bounds preserve 10 dB span");

    Effect upper;
    upper.set(14, 0);
    upper.set(2, -30); upper.set(3, -20);
    upper.set(6, -25); upper.set(7, -15);
    upper.run(0.001, 0.001);
    upper.set(14, -40);
    upper.run(0.001, 0.001);
    near(upper.get(2), -40.1, 1e-7, "attack close clamped at upper edge");
    near(upper.get(3), -40, 1e-7, "attack open clamped at upper edge");
    near(upper.get(6), -40.1, 1e-7, "release close clamped at upper edge");
    near(upper.get(7), -40, 1e-7, "release open clamped at upper edge");
}

static void test_gui_bounds()
{
    Effect e;
    near(e.get(11), 10, 1e-6, "attack time default");
    near(e.get(12), 120, 1e-6, "release time default");
    e.run(0.001, 0.001);
    std::vector<uint8_t> pixels(800 * 520 * 4);
    ysfx_gfx_config_t cfg{};
    cfg.pixel_width = 800; cfg.pixel_height = 520;
    cfg.pixels = pixels.data(); cfg.scale_factor = 1;
    ysfx_gfx_setup(e.fx, &cfg);
    ysfx_gfx_update_mouse(e.fx, 0, 0, 0, 0, 0, 0);
    ysfx_gfx_run(e.fx);
    e.set(11, 4);
    e.set(12, 20);
    // ATTACK / RELEASE の Time つまみ（RMS 2 つ、REDUCTION 1 つ、続いて各 2 つ）
    double kw = e.px("kw"), mg = e.px("mg"), gg = e.px("gg");
    for (int knob_x : {int(mg + 3 * kw + 2 * gg + kw / 2), int(mg + 5 * kw + 3 * gg + kw / 2)}) {
        int knob_y = static_cast<int>(e.px("kz_y") + 49);
        for (int click = 0; click < 2; ++click) {
            ysfx_gfx_update_mouse(e.fx, 0, knob_x, knob_y, 1, 0, 0);
            ysfx_gfx_run(e.fx);
            ysfx_gfx_update_mouse(e.fx, 0, knob_x, knob_y, 0, 0, 0);
            ysfx_gfx_run(e.fx);
        }
    }
    near(e.get(11), 10, 1e-6, "attack time reset");
    near(e.get(12), 120, 1e-6, "release time reset");
    if (const char *path = std::getenv("AZM_TEST_SCREENSHOT")) {
        ysfx_gfx_update_mouse(e.fx, 0, 0, 0, 0, 0, 0);
        ysfx_gfx_run(e.fx);
        FILE *file = std::fopen(path, "wb");
        check(file != nullptr, "open screenshot");
        std::fprintf(file, "P6\n800 520\n255\n");
        for (size_t i = 0; i < pixels.size(); i += 4) {
            std::fputc(pixels[i + 2], file);
            std::fputc(pixels[i + 1], file);
            std::fputc(pixels[i], file);
        }
        std::fclose(file);
    }
    int x = static_cast<int>(e.px("g_x0") + 30);
    int y = static_cast<int>(e.px("pl_b") + 13);
    ysfx_gfx_update_mouse(e.fx, 0, x, y, 1, 0, 0);
    ysfx_gfx_run(e.fx);
    ysfx_gfx_update_mouse(e.fx, 0, x + 40, y, 1, 0, 0);
    ysfx_gfx_run(e.fx);
    ysfx_gfx_update_mouse(e.fx, 0, x + 40, y, 0, 0, 0);
    ysfx_gfx_run(e.fx);
    near(e.get(13), -73, 1e-6, "drag meter minimum");
    e.run(0.001, 0.001);

    ysfx_gfx_update_mouse(e.fx, 0, x, y, 1, 0, 0);
    ysfx_gfx_run(e.fx);
    ysfx_gfx_update_mouse(e.fx, 0, x, y, 0, 0, 0);
    ysfx_gfx_run(e.fx);
    near(e.var("edit_id"), 13, 0, "click meter minimum opens editor");
    for (char c : {'-', '5', '0', '\n'}) {
        ysfx_gfx_add_key(e.fx, 0, c, true);
        ysfx_gfx_run(e.fx);
        ysfx_gfx_add_key(e.fx, 0, c, false);
    }
    near(e.get(13), -50, 1e-6, "type meter minimum");
    near(e.get(2), -50, 1e-6, "GUI narrowing clamps attack");
    near(e.get(6), -50, 1e-6, "GUI narrowing clamps release");
    check((ysfx_fetch_slider_automations(e.fx) & (uint64_t(1) << 1)) != 0,
          "GUI clamping notifies automation");
    int right_x = static_cast<int>(e.px("g_x0") + e.px("g_w") - 30);
    ysfx_gfx_update_mouse(e.fx, 0, right_x, y, 1, 0, 0);
    ysfx_gfx_run(e.fx);
    ysfx_gfx_update_mouse(e.fx, 0, right_x, y, 0, 0, 0);
    ysfx_gfx_run(e.fx);
    near(e.var("edit_id"), 14, 0, "click meter maximum opens editor");
    for (char c : {'-', '4', '0', '\n'}) {
        ysfx_gfx_add_key(e.fx, 0, c, true);
        ysfx_gfx_run(e.fx);
        ysfx_gfx_add_key(e.fx, 0, c, false);
    }
    near(e.get(14), -40, 1e-6, "type meter maximum");
    int plot_x = static_cast<int>(e.px("pl_x") + e.px("pl_w") * 0.7);
    int plot_y = static_cast<int>(e.px("pl_y") + e.px("pl_h") * 0.5);
    for (int click = 0; click < 2; ++click) {
        ysfx_gfx_update_mouse(e.fx, 0, plot_x, plot_y, 1, 0, 0);
        ysfx_gfx_run(e.fx);
        ysfx_gfx_update_mouse(e.fx, 0, plot_x, plot_y, 0, 0, 0);
        ysfx_gfx_run(e.fx);
    }
    near(e.get(13), -75, 1e-6, "double-click resets minimum");
    near(e.get(14), -35, 1e-6, "double-click resets maximum");
    near(e.get(2), -50, 1e-6, "reset does not restore clamped curve");
}

// The first sample initialises the gain directly to the selected curve's target.
static void test_curve_shapes()
{
    const double strengths[] = {-1, -0.8, -0.5, -0.1, 0, 0.1, 0.5, 0.8, 1};
    const double fractions[] = {-0.3, 0.01, 0.05, 0.25, 0.5, 0.75, 0.95, 0.99, 1.3};
    for (double c : strengths) {
        for (double f : fractions) {
            for (int sync = 0; sync < 2; ++sync) {
                Effect e;
                e.set(16, 0);
                e.set(5, sync);
                e.set(sync ? 4 : 8, c * 100);
                double close = sync ? -65 : -55, open = sync ? -55 : -40;
                double level_db = close + f * (open - close);
                double amplitude = std::pow(10.0, level_db / 20);
                e.run(amplitude, amplitude);
                near(e.var("gdb"), curve(level_db, close, open, c), 1e-6, "quadratic curve strengths");
            }
        }
    }
    double previous = -1e9;
    for (double level_db = -70; level_db <= -50; level_db += 0.25) {
        Effect e;
        e.set(16, 0); e.set(5, 1);
        e.set(4, 100);
        double amplitude = std::pow(10.0, level_db / 20);
        e.run(amplitude, amplitude);
        check(e.var("gdb") >= previous - 1e-9 && e.var("gdb") <= 1e-12 && e.var("gdb") >= -40 - 1e-12,
              "curve is monotonic and stays within range");
        previous = e.var("gdb");
    }
    Effect straight;
    straight.set(16, 0); straight.set(8, 0);
    double amplitude = std::pow(10.0, -47.5 / 20);
    straight.run(amplitude, amplitude);
    near(straight.var("gdb"), -20, 1e-9, "zero strength is a straight line");
}

static void test_defaults()
{
    Effect e;
    const double expected[][2] = {
        {1, 40}, {2, -65}, {3, -55}, {4, 50}, {6, -55}, {7, -40}, {8, -50},
        {9, 6}, {10, 40}, {11, 10}, {12, 120}, {15, 30}, {16, 60},
    };
    for (const auto &entry : expected)
        near(e.get(int(entry[0])), entry[1], 1e-9, "default value");
}

static void test_handle_drag()
{
    Effect e;
    e.run(0.001, 0.001);
    std::vector<uint8_t> pixels(800 * 520 * 4);
    ysfx_gfx_config_t cfg{};
    cfg.pixel_width = 800; cfg.pixel_height = 520;
    cfg.pixels = pixels.data(); cfg.scale_factor = 1;
    ysfx_gfx_setup(e.fx, &cfg);
    auto mouse = [&](int x, int y, int down) {
        ysfx_gfx_update_mouse(e.fx, 0, x, y, down, 0, 0);
        ysfx_gfx_run(e.fx);
    };
    mouse(0, 0, 0);
    // The handle sits at the curve midpoint (0.5 + c/4, 0.5 - c/4) of the Close/Open box.
    auto handle_px = [&](double c, double &px, double &py) {
        px = e.px("g_x0") + (-55 + (0.5 + c / 4) * 15 - e.var("v_lo")) * e.px("g_w") / e.var("v_span");
        py = e.px("g_y0") + 40 * (0.5 + c / 4) * e.px("g_h") / e.var("y_span");
    };
    auto current = [&]() { return e.get(8) / 100; };
    auto drag = [&](double px, double py, double dx, double dy) {
        e.set_var("lc_id", 0); // avoid treating back-to-back drags as a double-click
        mouse(int(px), int(py), 1);
        mouse(int(px + dx / 2), int(py + dy / 2), 1);
        mouse(int(px + dx), int(py + dy), 1);
        mouse(int(px + dx), int(py + dy), 0);
        e.run(0.001, 0.001);
    };

    double px, py;
    handle_px(current(), px, py);
    drag(px, py, 20, 0);
    double dfx = 20 * e.var("v_span") / (e.px("g_w") * 15);
    near(current(), -0.5 + 2 * dfx, 0.02, "dragging right opens the curve later");
    handle_px(current(), px, py);
    drag(px, py, 0, 20);
    double dfy = -20 * e.var("y_span") / (40 * e.px("g_h"));
    near(current(), -0.5 + 2 * dfx - 2 * dfy, 0.02, "dragging down opens the curve later");

    handle_px(current(), px, py);
    drag(px, py, 600, 600);
    near(current(), 1, 1e-9, "curve strength is capped at +100%");
    handle_px(current(), px, py);
    drag(px, py, -600, -600);
    near(current(), -1, 1e-9, "curve strength is capped at -100%");

    handle_px(current(), px, py);
    for (int click = 0; click < 2; ++click) {
        mouse(int(px), int(py), 1);
        mouse(int(px), int(py), 0);
    }
    near(current(), kReleaseCurve, 1e-9, "double-click resets the curve");
}

static void test_fields()
{
    Effect e;
    e.run(0.001, 0.001);
    std::vector<uint8_t> pixels(800 * 520 * 4);
    ysfx_gfx_config_t cfg{};
    cfg.pixel_width = 800; cfg.pixel_height = 520;
    cfg.pixels = pixels.data(); cfg.scale_factor = 1;
    ysfx_gfx_setup(e.fx, &cfg);
    auto mouse = [&](int x, int y, int down) {
        ysfx_gfx_update_mouse(e.fx, 0, x, y, down, 0, 0);
        ysfx_gfx_run(e.fx);
    };
    mouse(0, 0, 0);
    double g_x0 = e.px("g_x0"), g_w = e.px("g_w");
    double y = e.px("pl_b") + 27 + 8;
    double grp = (g_w - 12) / 2, w = (grp - 50 - 12) / 3;
    // Field centres: Curve, Close, Open of the left (Attack) and right (Release) groups.
    auto cx = [&](bool release, int k) {
        return int((release ? g_x0 + g_w - grp : g_x0) + 54 + (w + 4) * k + w / 2);
    };
    auto drag = [&](int x, int dx, int dy) {
        mouse(x, int(y), 1);
        mouse(x + dx, int(y) + dy, 1);
        mouse(x + dx, int(y) + dy, 0);
    };
    drag(cx(false, 0), 20, 0);
    near(e.get(4), 60, 1e-9, "dragging the attack curve field");
    drag(cx(false, 1), 0, -20);
    near(e.get(2), -64, 1e-6, "dragging the attack close field");
    drag(cx(true, 2), -20, 0);
    near(e.get(7), -41, 1e-6, "dragging the release open field");

    mouse(cx(true, 0), int(y), 1);
    mouse(cx(true, 0), int(y), 0);
    near(e.var("edit_id"), 8, 0, "click release curve field opens editor");
    for (char c : {'-', '8', '0', '\n'}) {
        ysfx_gfx_add_key(e.fx, 0, c, true);
        ysfx_gfx_run(e.fx);
        ysfx_gfx_add_key(e.fx, 0, c, false);
    }
    near(e.get(8), -80, 1e-9, "type release curve");

    e.set(5, 1);
    e.run(0.001, 0.001);
    drag(cx(true, 0), 20, 0);
    near(e.get(8), -80, 1e-9, "release fields are inactive while synced");
}

int main()
{
    compare_reference(0, 0, false);
    compare_reference(1, 1, false);
    compare_reference(0, 1, false);
    compare_reference(1, 0, false);
    compare_reference(0.5, 0.75, true);
    test_direction_and_automation();
    test_detector_and_gain();
    test_clamping();
    test_block_sizes();
    test_bypass();
    test_gui_bounds();
    test_defaults();
    test_curve_shapes();
    test_handle_drag();
    test_fields();
    std::puts("ysfx integration tests passed");
}
