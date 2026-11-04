// Integration tests for the actual JSFX, using the ysfx library.
#include "ysfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
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

// Fixed parameter set used by the DSP tests: Attack Open -55 / Range 10 (Close -65),
// Release Close -55 / Range 15 (Open -40), Range 40 dB.
static void set_reference_params(Effect &e)
{
    e.set(1, 40);
    e.set(2, 10); e.set(3, -55); e.set(4, kAttackCurve * 100);
    e.set(6, -55); e.set(7, 15); e.set(8, kReleaseCurve * 100);
    e.set(9, 6); e.set(10, 40); e.set(11, 10); e.set(12, 120);
}

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
    set_reference_params(e);
    e.set(13, attack_mix * 100);
    e.set(14, release_mix * 100);
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
    e.set(13, 0); e.set(14, 100);
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
    e.set(13, 0); e.set(14, 0); e.set(10, 40);
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
    e.set(15, 1);
    for (int i = 0; i < 4800; ++i) l = e.run(0.001, 0.001);
    near(l, 0.001, 1e-9, "bypass passes input through");
    e.set(15, 0);
    for (int i = 0; i < 4800; ++i) l = e.run(0.001, 0.001);
    check(std::abs(l) < 0.001 * 0.5, "bypass off restores processing");
}

static void test_relative_ranges()
{
    Effect e;
    e.set(3, -50); e.set(2, 12);
    e.set(6, -40); e.set(7, 20);
    e.run(0.001, 0.001);
    near(e.var("a_close"), -62, 1e-9, "attack Close = Open - Range");
    near(e.var("r_open"), -20, 1e-9, "release Open = Close + Range");
    near(e.var("e_close"), -40, 1e-9, "release curve close");
    near(e.var("e_open"), -20, 1e-9, "release curve open");
    e.set(5, 1);
    e.run(0.001, 0.001);
    near(e.var("e_close"), -62, 1e-9, "synced release uses the attack Close");
    near(e.var("e_open"), -50, 1e-9, "synced release uses the attack Open");
    near(e.get(6), -40, 1e-9, "sync keeps the stored release values");
    near(e.get(7), 20, 1e-9, "sync keeps the stored release range");

    // Moving the base moves the whole curve; the Range stays.
    e.set(3, -30);
    e.run(0.001, 0.001);
    near(e.var("a_close"), -42, 1e-9, "attack curve moves with Open");
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
                set_reference_params(e);
                e.set(14, 0);
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
        set_reference_params(e);
        e.set(14, 0); e.set(5, 1);
        e.set(4, 100);
        double amplitude = std::pow(10.0, level_db / 20);
        e.run(amplitude, amplitude);
        check(e.var("gdb") >= previous - 1e-9 && e.var("gdb") <= 1e-12 && e.var("gdb") >= -40 - 1e-12,
              "curve is monotonic and stays within range");
        previous = e.var("gdb");
    }
    Effect straight;
    set_reference_params(straight);
    straight.set(14, 0); straight.set(8, 0);
    double amplitude = std::pow(10.0, -47.5 / 20);
    straight.run(amplitude, amplitude);
    near(straight.var("gdb"), -20, 1e-9, "zero strength is a straight line");
}

static void test_defaults()
{
    Effect e;
    const double expected[][2] = {
        {1, 60}, {2, 10}, {3, -55}, {4, 60}, {6, -55}, {7, 15}, {8, -60},
        {9, 6}, {10, 60}, {11, 10}, {12, 120}, {13, 30}, {14, 60}, {16, 0},
    };
    for (const auto &entry : expected)
        near(e.get(int(entry[0])), entry[1], 1e-9, "default value");
}

// GUI harness: 940x520 pixel buffer and mouse/key helpers (coordinates are in mouse units).
struct Gui : Effect {
    std::vector<uint8_t> pixels = std::vector<uint8_t>(940 * 520 * 4);
    Gui()
    {
        set(1, 40);
        set(2, 10); set(3, -55); set(4, 50);
        set(6, -55); set(7, 15); set(8, -50);
        run(0.001, 0.001);
        ysfx_gfx_config_t cfg{};
        cfg.pixel_width = 940; cfg.pixel_height = 520;
        cfg.pixels = pixels.data(); cfg.scale_factor = 1;
        ysfx_gfx_setup(fx, &cfg);
        mouse(0, 0, 0);
    }
    void mouse(int x, int y, int down)
    {
        ysfx_gfx_update_mouse(fx, 0, x, y, down, 0, 0);
        ysfx_gfx_run(fx);
    }
    void click(int x, int y)
    {
        mouse(x, y, 1);
        mouse(x, y, 0);
    }
    void drag(double x, double y, double dx, double dy)
    {
        set_var("lc_id", 0); // avoid treating back-to-back drags as a double-click
        mouse(int(x), int(y), 1);
        mouse(int(x + dx / 2), int(y + dy / 2), 1);
        mouse(int(x + dx), int(y + dy), 1);
        mouse(int(x + dx), int(y + dy), 0);
        run(0.001, 0.001);
    }
    void type(const char *text)
    {
        for (const char *c = text; *c; ++c) {
            ysfx_gfx_add_key(fx, 0, *c, true);
            ysfx_gfx_run(fx);
            ysfx_gfx_add_key(fx, 0, *c, false);
        }
    }
    // Let the eased graph axis settle.
    void settle(int frames = 60)
    {
        for (int i = 0; i < frames; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            mouse(0, 0, 0);
        }
    }
    double xpix(double db) { return px("g_x0") + (db - var("v_lo")) * px("g_w") / var("v_span"); }
    double ypix(double gain) { return px("g_y0") - gain * px("g_h") / var("y_span"); }
};

static void test_axis_and_screenshot()
{
    Gui e;
    // Attack Close -65 .. Release Open -40, plus 10 dB on each side.
    near(e.var("v_lo"), -75, 1e-9, "axis low = lowest Close - 10 dB");
    near(e.var("v_hi"), -30, 1e-9, "axis high = highest Open + 10 dB");

    e.set(3, -45);
    e.settle();
    near(e.var("v_lo"), -65, 0.02, "axis follows Close (attack -55, release -55)");
    near(e.var("v_hi"), -30, 0.02, "axis follows Open (release -40)");
    e.set(5, 1);
    e.settle();
    near(e.var("v_hi"), -35, 0.02, "synced axis ignores the stored release curve");

    // Live detection: meters track the combined Peak/RMS levels of each side.
    e.set(5, 0); e.set(13, 0); e.set(14, 0);
    for (int i = 0; i < 200; ++i) e.run(0.001, 0.001);
    e.mouse(0, 0, 0);
    near(e.var("ui_a"), -60, 0.05, "attack meter shows the combined level");
    near(e.var("ui_r"), -60, 0.05, "release meter shows the combined level");
    near(e.var("m_a"), 0, 0, "GUI consumes the audio-thread maximum");
    e.set_var("dir_up", 1); e.set_var("m_up", 1);
    e.settle(30);
    check(e.var("ui_hl") > 0.95, "attack side highlighted while the level rises");

    if (const char *path = std::getenv("AZM_TEST_SCREENSHOT")) {
        e.set(3, -55);
        e.set(13, 30); e.set(14, 60);
        e.settle();
        for (int i = 0; i < 30; ++i) e.run(0.0012, 0.0012);
        e.mouse(0, 0, 0);
        FILE *file = std::fopen(path, "wb");
        check(file != nullptr, "open screenshot");
        std::fprintf(file, "P6\n940 520\n255\n");
        for (size_t i = 0; i < e.pixels.size(); i += 4) {
            std::fputc(e.pixels[i + 2], file);
            std::fputc(e.pixels[i + 1], file);
            std::fputc(e.pixels[i], file);
        }
        std::fclose(file);
    }
}

// Detection bars: lines in the curve colours with the same glow, constant and behind the curves (no
// labels), fading out near the graph edge; the curve that decides the target gain glows as well.
static void test_detection_bars()
{
    Gui e;
    e.settle();
    auto px = [&](double x, double y, int ch) { return int(e.pixels[(size_t(y) * 940 + size_t(x)) * 4 + ch]); };
    auto red = [&](double x, double y) { return px(x, y, 2); };
    auto peak = [&](double x, double y) {
        int m = 0;
        for (int d = -2; d <= 2; ++d) m = std::max(m, red(x + d, y));
        return m;
    };
    auto blue = [&](double x, double y) { return px(x, y, 0); };
    // The displayed level decays slightly between frames, so positions are read back after each frame.
    auto frame = [&](double a, double up) {
        e.set_var("m_a", 0); e.set_var("m_r", 0);
        e.set_var("ui_a", a); e.set_var("ui_r", -120);
        e.set_var("m_up", up); e.set_var("ui_hl", up);
        e.mouse(0, 0, 0);
        return e.xpix(e.var("ui_a"));
    };
    double y = e.px("pl_y") + 120;

    double x1 = frame(-47, 1);
    int bar = peak(x1, y);
    check(bar > 200, "attack bar is crisp at the detected level");
    check(red(x1 + 14, y) < 100 && red(x1 - 14, y) < 100, "no wide fill beside the bar");
    check(red(x1 + 15, e.px("pl_y") + 32 + 7) < 100, "no label chip next to the bar");
    check(red(x1 + 6, y) > red(x1 + 14, y) + 10, "the bar has a glow around it");
    double x1i = frame(-47, 0);
    check(std::abs(peak(x1i, y) - bar) <= 10, "the bar looks the same whichever side decides the gain");

    // Curves are drawn over the bars: the Release curve's flat bottom hides the bar where they meet.
    double x5 = frame(-60, 1);
    check(blue(x5, e.px("g_b")) > 200 && blue(x5, y) < 120, "the bar is behind the curves");

    frame(e.var("v_lo") - 5, 1);
    check(red(e.px("g_x0"), y) < 100, "bar outside the graph is not drawn at the edge");
    double x3 = frame(e.var("v_lo") + 1, 1);
    int near_edge = peak(x3, y);
    double x4 = frame(-47, 1);
    check(near_edge < peak(x4, y), "bar fades out as it approaches the edge");

    // Glow around the bottom (Close) segment shared by both curves: amber for Attack, white for Release.
    double gx = e.px("g_x0") + 20, gy = e.px("g_b") - 5;
    double far_y = gy - 30;
    frame(e.var("v_lo") - 5, 1);
    check(blue(gx, gy) < 95 && red(gx, gy) > red(gx, far_y) + 20, "attack curve glows while it decides the gain");
    frame(e.var("v_lo") - 5, 0);
    check(blue(gx, gy) > 105, "release curve glows while it decides the gain");
}

static void test_range_field()
{
    Gui e;
    double mg = e.px("mg"), cw = e.px("ui_w") - 2 * mg;
    int fx = int(mg + cw - 56);
    int fy = int(e.px("pl_b") + 12 + 10);
    e.set(1, 60);
    e.drag(fx, fy, 0, -40);
    check(e.get(1) > 60, "dragging up increases the Reduction");
    e.click(fx, fy);
    near(e.var("edit_id"), 1, 0, "click the Reduction field opens the editor");
    e.type("-45\n");
    near(e.get(1), 45, 1e-9, "typed Reduction is the (negative) lower limit");
}

static void test_collapse_and_knobs()
{
    Gui e;
    near(e.get(16), 0, 0, "RMS Control starts closed");
    e.set(16, 1);
    e.mouse(0, 0, 0);
    e.set(11, 4);
    e.set(12, 20);
    double kw = e.px("kw"), mg = e.px("mg"), gg = e.px("gg");
    // DETECTION (9, 10), ATTACK (13, 11), RELEASE (14, 12), two knobs each.
    int ky = int(e.px("kg_y") + 14 + 35);
    int attack_time_x = int(mg + 3 * kw + gg + kw / 2);
    int release_time_x = int(mg + 5 * kw + 2 * gg + kw / 2);
    for (int x : {attack_time_x, release_time_x}) {
        e.click(x, ky);
        e.click(x, ky);
    }
    near(e.get(11), 10, 1e-6, "attack smoothing time reset");
    near(e.get(12), 120, 1e-6, "release smoothing time reset");

    e.drag(int(mg + 2 * kw + gg + kw / 2), ky, 0, -30);
    check(e.get(13) > 30, "dragging the Attack RMS Ratio knob up increases it");
    e.drag(int(mg + kw / 2), ky, 0, 30);
    check(e.get(9) < 6, "dragging the Sens knob down decreases it");

    double full_h = e.px("pl_b");
    e.click(int(mg + 10), int(e.px("kz_y") + 9));
    near(e.get(16), 0, 0, "header click collapses RMS Control");
    e.mouse(0, 0, 0);
    check(e.px("pl_b") > full_h + 50, "graph grows when the section is collapsed");
    e.click(attack_time_x, ky);
    near(e.get(11), 10, 1e-6, "collapsed knobs are not interactive");
    e.click(int(mg + 10), int(e.px("kz_y") + 9));
    near(e.get(16), 1, 0, "header click expands RMS Control");
    e.mouse(0, 0, 0);
    near(e.px("pl_b"), full_h, 1e-6, "layout is restored");
}

static void test_handle_drag()
{
    Gui e;
    // The handle sits at the curve midpoint (0.5 + c/4, 0.5 - c/4) of the Close/Open box.
    auto mid = [&](double c, double &x, double &y) {
        x = e.xpix(-55 + (0.5 + c / 4) * 15);
        y = e.ypix(-40 * (0.5 + c / 4));
    };
    auto current = [&]() { return e.get(8) / 100; };

    double x, y;
    mid(current(), x, y);
    e.drag(x, y, 20, 0);
    double dfx = 20 * e.var("v_span") / (e.px("g_w") * 15);
    near(current(), -0.5 + 2 * dfx, 0.02, "dragging right opens the curve later");
    mid(current(), x, y);
    e.drag(x, y, 0, 20);
    double dfy = -20 * e.var("y_span") / (40 * e.px("g_h"));
    near(current(), -0.5 + 2 * dfx - 2 * dfy, 0.02, "dragging down opens the curve later");

    mid(current(), x, y);
    e.drag(x, y, 600, 600);
    near(current(), 1, 1e-9, "curve strength is capped at +100%");
    mid(current(), x, y);
    e.drag(x, y, -600, -600);
    near(current(), -1, 1e-9, "curve strength is capped at -100%");

    mid(current(), x, y);
    for (int click = 0; click < 2; ++click) e.click(int(x), int(y));
    near(current(), -0.6, 1e-9, "double-click resets the curve");
}

static void test_base_and_range_handles()
{
    Gui e;
    double span = e.var("v_span"), gw = e.px("g_w");
    double db_per_px = span / gw;
    double v_lo = e.var("v_lo"), v_hi = e.var("v_hi");

    // Attack Open (103) moves the whole curve; Close follows at the same Range.
    double hx = e.xpix(-55), hy = e.ypix(0);
    e.set_var("lc_id", 0);
    e.mouse(int(hx), int(hy), 1);
    e.mouse(int(hx + 20), int(hy), 1);
    near(e.get(3), -55 + 20 * db_per_px, 0.1, "attack Open handle moves the curve");
    near(e.get(2), 10, 1e-9, "attack Range is unchanged");
    near(e.var("v_lo"), v_lo, 1e-9, "axis does not move while a handle is dragged (low)");
    near(e.var("v_hi"), v_hi, 1e-9, "axis does not move while a handle is dragged (high)");
    e.mouse(int(hx + 20), int(hy), 0);
    e.set(3, -55);

    // Attack Close (102) edits Range: moving left widens it.
    e.run(0.001, 0.001);
    e.settle();
    e.drag(e.xpix(-65), e.ypix(-40), -20, 0);
    near(e.get(2), 10 + 20 * db_per_px, 0.15, "attack Close handle edits the Range");
    near(e.get(3), -55, 1e-9, "attack Open stays");
    e.set(2, 10);

    // Release Close (106) moves the whole curve; Open (107) edits Range.
    e.run(0.001, 0.001);
    e.settle();
    e.drag(e.xpix(-55), e.ypix(-40), 20, 0);
    near(e.get(6), -55 + 20 * db_per_px, 0.15, "release Close handle moves the curve");
    near(e.get(7), 15, 1e-9, "release Range is unchanged");
    e.set(6, -55);
    e.run(0.001, 0.001);
    e.settle();
    e.drag(e.xpix(-40), e.ypix(0), 20, 0);
    near(e.get(7), 15 + 20 * db_per_px, 0.15, "release Open handle edits the Range");
    // Release Open never goes above 0 dB.
    e.settle();
    e.drag(e.xpix(-55 + e.get(7)), e.ypix(0), 1500, 0);
    near(e.get(7), 55, 1e-6, "release Open is limited to 0 dB");
}

static void test_fields()
{
    Gui e;
    double g_x0 = e.px("mg"), g_w = e.px("ui_w") - 2 * e.px("mg");
    double y = e.px("pl_b") + 12 + 10;
    double grp = (g_w - 104 - 60) / 2, w = (grp - 52 - 9) / 3;
    // Field centres: Curve, Open|Close, Range of the left (Attack) and right (Release) groups.
    auto cx = [&](bool release, int k) {
        const double width[] = {0.85 * w, w, 1.15 * w};
        double x = (release ? g_x0 + grp + 30 : g_x0) + 55;
        for (int i = 0; i < k; ++i) x += width[i] + 3;
        return int(x + width[k] / 2);
    };
    e.drag(cx(false, 0), y, 20, 0);
    near(e.get(4), 60, 1e-9, "dragging the attack curve field");
    e.drag(cx(false, 1), y, 0, -20);
    near(e.get(3), -54, 1e-6, "dragging the attack Open field");
    near(e.get(2), 10, 1e-9, "Range is independent of the Open field");
    e.drag(cx(false, 2), y, 0, -20);
    near(e.get(2), 9, 1e-6, "dragging the attack Range field (shown as a negative value)");
    e.drag(cx(true, 1), y, 20, 0);
    near(e.get(6), -54, 1e-6, "dragging the release Close field");
    e.drag(cx(true, 2), y, -20, 0);
    near(e.get(7), 14, 1e-6, "dragging the release Range field");

    e.click(cx(true, 0), int(y));
    near(e.var("edit_id"), 8, 0, "click release curve field opens editor");
    e.type("-80\n");
    near(e.get(8), -80, 1e-9, "type release curve");

    e.click(cx(false, 2), int(y));
    near(e.var("edit_id"), 2, 0, "click attack Range field opens editor");
    e.type("-20\n");
    near(e.get(2), 20, 1e-9, "typed Range is a distance below Open");

    e.set(5, 1);
    e.run(0.001, 0.001);
    e.drag(cx(true, 0), y, 20, 0);
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
    test_relative_ranges();
    test_block_sizes();
    test_bypass();
    test_defaults();
    test_curve_shapes();
    test_axis_and_screenshot();
    test_detection_bars();
    test_range_field();
    test_collapse_and_knobs();
    test_handle_drag();
    test_base_and_range_handles();
    test_fields();
    std::puts("ysfx integration tests passed");
}
