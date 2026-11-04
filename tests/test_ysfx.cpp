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

struct Shape { double x1, y1, x2, y2; };
static const Shape kAttackShape{0.5, 0.167, 0.833, 0.5};
static const Shape kReleaseShape{0.167, 0.5, 0.5, 0.833};

static double bezier(double u, double p1, double p2)
{
    return u * (3 * (1 - u) * ((1 - u) * p1 + u * p2) + u * u);
}

static double curve(double level, double close, double open, Shape s, double range = 30)
{
    double t = (level - close) / (open - close);
    if (t <= 0) return -range;
    if (t >= 1) return 0;
    double lo = 0, hi = 1;
    for (int i = 0; i < 100; ++i) {
        double m = (lo + hi) / 2;
        (bezier(m, s.x1, s.x2) < t ? lo : hi) = m;
    }
    double u = (lo + hi) / 2;
    return -range * (1 - bezier(u, s.y1, s.y2));
}

static void compare_reference(double attack_mix, double release_mix, bool sync)
{
    Effect e;
    e.set(19, attack_mix * 100);
    e.set(20, release_mix * 100);
    e.set(5, sync ? 1 : 0);
    double msq = 0, gain = 0;
    double dir_a_f = 0, dir_r_f = 0, prev_dir_a = 0, prev_dir_r = 0;
    bool up = false;
    const double rms_k = 1 - std::exp(-1.0 / (0.020 * 48000));
    const double dir_k = 1 - std::exp(-1.0 / (0.002 * 48000));
    const double open_k = 1 - std::exp(-1.0 / (0.004 * 48000));
    const double close_k = 1 - std::exp(-1.0 / (0.080 * 48000));
    const double peak_gain = std::pow(10.0, -3.0 / 20);
    for (int i = 0; i < 16000; ++i) {
        double envelope = 0.0002 + 0.0015 * (1 + std::sin(i * 0.0013));
        double left = envelope * std::sin(i * 0.13);
        double right = envelope * 0.7 * std::cos(i * 0.11);
        double a = std::max(std::abs(left), std::abs(right));
        msq = std::max(msq + rms_k * (a * a - msq), 1e-24);
        double rms = std::sqrt(2 * msq);
        auto mix = [&](double peak, double r, double ratio) {
            return std::max(peak * peak_gain * (1 - ratio) + r * ratio, 1e-12);
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
        double target = up || sync ? curve(level, -70, -60, kAttackShape)
                                   : curve(level, -65, -55, kReleaseShape);
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
    e.set(19, 0); e.set(20, 100);
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
    e.set(19, 0); e.set(20, 0);
    e.run(0.1, 0.1);
    double previous_msq = e.var("msq");
    e.run(0, 0);
    near(e.var("pk_lin"), 0, 0, "peak has no release envelope");
    double rms_k = 1 - std::exp(-1.0 / (0.020 * 48000));
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

static void test_legacy_controls_do_not_affect_audio()
{
    Effect standard, legacy;
    legacy.set(10, 500);
    legacy.set(13, 1000);
    legacy.set(18, 10);
    for (int i = 0; i < 4000; ++i) {
        double sample = 0.001 * std::sin(i * 0.08);
        near(legacy.run(sample, sample), standard.run(sample, sample), 1e-12,
             "legacy controls are inactive");
    }
}

static void test_old_state()
{
    Effect e;
    ysfx_state_slider_t old_sliders[] = {
        {8, 5}, {9, 400}, {10, -2}, {13, 7}, {14, 45}, {17, 8}
    };
    uint8_t empty = 0;
    ysfx_state_t state{};
    state.sliders = old_sliders;
    state.slider_count = sizeof(old_sliders) / sizeof(old_sliders[0]);
    state.data = &empty;
    check(ysfx_load_state(e.fx, &state), "load old parameter state");
    e.run(0.001, 0.001);
    near(e.get(9), 5, 0, "old peak sensitivity preserved");
    near(e.get(11), -2, 0, "old RMS sensitivity preserved");
    near(e.get(14), 7, 0, "old open time preserved");
    near(e.get(15), 45, 0, "old close time preserved");
    near(e.get(19), 0, 0, "new attack ratio defaults");
    near(e.get(20), 100, 0, "new release ratio defaults");
}

static void test_clamping()
{
    Effect e;
    e.set(5, 1);
    e.set(16, -50);
    e.run(0.001, 0.001);
    near(e.get(2), -50, 1e-7, "attack close clamped");
    near(e.get(3), -49.9, 1e-7, "attack open separated");
    near(e.get(6), -50, 1e-7, "synced release close clamped");
    near(e.get(7), -49.9, 1e-7, "synced release open separated");
    e.set(16, -90);
    e.run(0.001, 0.001);
    near(e.get(2), -50, 1e-7, "widening does not restore values");
    e.set(2, -80);
    e.set(3, -75);
    e.run(0.001, 0.001);
    near(e.get(2), -80, 1e-7, "automation remains within viewport");
    e.set(17, -60);
    e.set(16, -30);
    e.run(0.001, 0.001);
    check(e.get(17) - e.get(16) >= 10 - 1e-8, "automated bounds preserve 10 dB span");

    Effect upper;
    upper.set(17, 0);
    upper.set(2, -30); upper.set(3, -20);
    upper.set(6, -25); upper.set(7, -15);
    upper.run(0.001, 0.001);
    upper.set(17, -40);
    upper.run(0.001, 0.001);
    near(upper.get(2), -40.1, 1e-7, "attack close clamped at upper edge");
    near(upper.get(3), -40, 1e-7, "attack open clamped at upper edge");
    near(upper.get(6), -40.1, 1e-7, "release close clamped at upper edge");
    near(upper.get(7), -40, 1e-7, "release open clamped at upper edge");
}

static void test_gui_bounds()
{
    Effect e;
    near(e.get(14), 4, 1e-6, "attack time default");
    near(e.get(15), 80, 1e-6, "release time default");
    e.run(0.001, 0.001);
    std::vector<uint8_t> pixels(800 * 520 * 4);
    ysfx_gfx_config_t cfg{};
    cfg.pixel_width = 800; cfg.pixel_height = 520;
    cfg.pixels = pixels.data(); cfg.scale_factor = 1;
    ysfx_gfx_setup(e.fx, &cfg);
    ysfx_gfx_update_mouse(e.fx, 0, 0, 0, 0, 0, 0);
    ysfx_gfx_run(e.fx);
    e.set(14, 10);
    e.set(15, 20);
    for (int knob_x : {454, 651}) {
        int knob_y = static_cast<int>(e.var("kz_y") + 49);
        for (int click = 0; click < 2; ++click) {
            ysfx_gfx_update_mouse(e.fx, 0, knob_x, knob_y, 1, 0, 0);
            ysfx_gfx_run(e.fx);
            ysfx_gfx_update_mouse(e.fx, 0, knob_x, knob_y, 0, 0, 0);
            ysfx_gfx_run(e.fx);
        }
    }
    near(e.get(14), 4, 1e-6, "attack time reset");
    near(e.get(15), 80, 1e-6, "release time reset");
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
    int x = static_cast<int>(e.var("g_x0") + 30);
    int y = static_cast<int>(e.var("pl_b") + 13);
    ysfx_gfx_update_mouse(e.fx, 0, x, y, 1, 0, 0);
    ysfx_gfx_run(e.fx);
    ysfx_gfx_update_mouse(e.fx, 0, x + 40, y, 1, 0, 0);
    ysfx_gfx_run(e.fx);
    ysfx_gfx_update_mouse(e.fx, 0, x + 40, y, 0, 0, 0);
    ysfx_gfx_run(e.fx);
    near(e.get(16), -73, 1e-6, "drag meter minimum");
    e.run(0.001, 0.001);

    ysfx_gfx_update_mouse(e.fx, 0, x, y, 1, 0, 0);
    ysfx_gfx_run(e.fx);
    ysfx_gfx_update_mouse(e.fx, 0, x, y, 0, 0, 0);
    ysfx_gfx_run(e.fx);
    near(e.var("edit_id"), 16, 0, "click meter minimum opens editor");
    for (char c : {'-', '6', '0', '\n'}) {
        ysfx_gfx_add_key(e.fx, 0, c, true);
        ysfx_gfx_run(e.fx);
        ysfx_gfx_add_key(e.fx, 0, c, false);
    }
    near(e.get(16), -60, 1e-6, "type meter minimum");
    near(e.get(2), -60, 1e-6, "GUI narrowing clamps attack");
    near(e.get(6), -60, 1e-6, "GUI narrowing clamps release");
    check((ysfx_fetch_slider_automations(e.fx) & (uint64_t(1) << 1)) != 0,
          "GUI clamping notifies automation");
    int right_x = static_cast<int>(e.var("g_x0") + e.var("g_w") - 30);
    ysfx_gfx_update_mouse(e.fx, 0, right_x, y, 1, 0, 0);
    ysfx_gfx_run(e.fx);
    ysfx_gfx_update_mouse(e.fx, 0, right_x, y, 0, 0, 0);
    ysfx_gfx_run(e.fx);
    near(e.var("edit_id"), 17, 0, "click meter maximum opens editor");
    for (char c : {'-', '4', '0', '\n'}) {
        ysfx_gfx_add_key(e.fx, 0, c, true);
        ysfx_gfx_run(e.fx);
        ysfx_gfx_add_key(e.fx, 0, c, false);
    }
    near(e.get(17), -40, 1e-6, "type meter maximum");
    int plot_x = static_cast<int>(e.var("pl_x") + e.var("pl_w") * 0.7);
    int plot_y = static_cast<int>(e.var("pl_y") + e.var("pl_h") * 0.5);
    for (int click = 0; click < 2; ++click) {
        ysfx_gfx_update_mouse(e.fx, 0, plot_x, plot_y, 1, 0, 0);
        ysfx_gfx_run(e.fx);
        ysfx_gfx_update_mouse(e.fx, 0, plot_x, plot_y, 0, 0, 0);
        ysfx_gfx_run(e.fx);
    }
    near(e.get(16), -75, 1e-6, "double-click resets minimum");
    near(e.get(17), -45, 1e-6, "double-click resets maximum");
    near(e.get(2), -60, 1e-6, "reset does not restore clamped curve");
}

static void set_shape(Effect &e, int x1, int y1, int x2, int y2, Shape s)
{
    e.set(x1, s.x1 * 100); e.set(y1, s.y1 * 100);
    e.set(x2, s.x2 * 100); e.set(y2, s.y2 * 100);
}

// The first sample initialises the gain directly to the selected curve's target.
static void test_curve_shapes()
{
    const Shape shapes[] = {
        {0.1, 0.9, 0.9, 0.1}, {0.9, 0.1, 0.1, 0.9}, {0, 0, 1, 1}, {1, 0, 0, 1},
        {0.3, 0.8, 0.7, 0.2}, {0, 1, 1, 0}, {0.5, 0.5, 0.5, 0.5}, {1, 1, 0, 0},
    };
    for (const Shape &s : shapes) {
        for (double level_db : {-69.0, -64.0, -62.0, -58.5, -56.0, -51.0, -45.0}) {
            for (int sync = 0; sync < 2; ++sync) {
                Effect e;
                e.set(20, 0);
                e.set(5, sync);
                if (sync) set_shape(e, 4, 21, 22, 23, s);
                else set_shape(e, 8, 24, 25, 26, s);
                double amplitude = std::pow(10.0, (level_db + 3) / 20);
                e.run(amplitude, amplitude);
                double close = sync ? -70 : -65, open = sync ? -60 : -55;
                double expected = curve(level_db, close, open, s);
                // x'(u) touches zero for x1=1, x2=0, so the solved u is only accurate to about 1e-4 there.
                near(e.var("gdb"), expected, s.x1 == 1 && s.x2 == 0 ? 5e-3 : 1e-6,
                     "S-shaped and extreme curves");
            }
        }
    }
    double previous = -1e9;
    for (double level_db = -72; level_db <= -58; level_db += 0.25) {
        Effect e;
        e.set(20, 0); e.set(5, 1);
        set_shape(e, 4, 21, 22, 23, {0.8, 0.0, 0.2, 1.0});
        double amplitude = std::pow(10.0, (level_db + 3) / 20);
        e.run(amplitude, amplitude);
        check(e.var("gdb") >= previous - 1e-9 && e.var("gdb") <= 1e-12 && e.var("gdb") >= -30 - 1e-12,
              "curve is monotonic and stays within range");
        previous = e.var("gdb");
    }
}

static double shape_handle(const Shape &s, int handle, bool y)
{
    double u = handle == 1 ? 1.0 / 3 : 2.0 / 3;
    return y ? bezier(u, s.y1, s.y2) : bezier(u, s.x1, s.x2);
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
    auto handle_px = [&](Shape s, int handle, double &px, double &py) {
        double fx = shape_handle(s, handle, false), fy = shape_handle(s, handle, true);
        px = e.var("g_x0") + (-65 + fx * 10 - e.var("v_lo")) * e.var("g_w") / e.var("v_span");
        py = e.var("g_y0") + 30 * (1 - fy) * e.var("g_h") / e.var("y_span");
    };
    auto current = [&]() {
        return Shape{e.get(8) / 100, e.get(24) / 100, e.get(25) / 100, e.get(26) / 100};
    };
    auto drag = [&](double px, double py, double dx, double dy) {
        mouse(int(px), int(py), 1);
        mouse(int(px + dx / 2), int(py + dy / 2), 1);
        mouse(int(px + dx), int(py + dy), 1);
        mouse(int(px + dx), int(py + dy), 0);
        e.run(0.001, 0.001);
    };
    auto valid = [&](Shape s) {
        for (double v : {s.x1, s.y1, s.x2, s.y2})
            check(v >= -1e-9 && v <= 1 + 1e-9, "control points stay inside the unit square");
    };

    double px, py, nx, ny;
    Shape before = current();
    handle_px(before, 1, px, py);
    drag(px, py, 6, -10);
    Shape after = current();
    valid(after);
    handle_px(after, 1, nx, ny);
    near(nx, px + 6, 0.5, "handle 1 follows the pointer horizontally");
    near(ny, py - 10, 0.5, "handle 1 follows the pointer vertically");
    handle_px(before, 2, px, py);
    handle_px(after, 2, nx, ny);
    near(nx, px, 0.5, "handle 2 stays in place horizontally");
    near(ny, py, 0.5, "handle 2 stays in place vertically");

    handle_px(after, 2, px, py);
    drag(px, py, 600, -600);
    valid(current());
    handle_px(current(), 2, px, py);
    drag(px, py, -600, 600);
    valid(current());
    handle_px(current(), 1, px, py);
    drag(px, py, 600, 600);
    valid(current());
    handle_px(current(), 1, px, py);
    drag(px, py, -600, -600);
    valid(current());

    handle_px(current(), 2, px, py);
    for (int click = 0; click < 2; ++click) {
        mouse(int(px), int(py), 1);
        mouse(int(px), int(py), 0);
    }
    Shape reset = current();
    near(reset.x1, kReleaseShape.x1, 1e-9, "double-click resets the curve");
    near(reset.y2, kReleaseShape.y2, 1e-9, "double-click resets the curve");
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
    test_legacy_controls_do_not_affect_audio();
    test_old_state();
    test_gui_bounds();
    test_curve_shapes();
    test_handle_drag();
    std::puts("ysfx integration tests passed");
}
