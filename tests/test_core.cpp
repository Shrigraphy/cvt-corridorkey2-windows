// ---------------------------------------------------------------------------
// CorridorKey 2 for Resolve - tests for the host-free parts
//
// From Creative Video Tips (CVT)  |  https://creativevideotips.com
// Licence: CC BY-NC-SA 4.0 with the CorridorKey additional terms (see LICENSE).
//
//   ck2_tests                     unit tests
//   ck2_tests write <out.exr>     writes a 7x5 test pattern (for the Python check)
//   ck2_tests read <in.exr>       prints size and a few pixels of an EXR
// ---------------------------------------------------------------------------
#include "ck2_core.h"
#include "ck2_exr.h"
#include "ck2_job.h"

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static std::vector<float> pattern(int w, int h) {
    std::vector<float> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = x * 0.1f; p[1] = y * 0.2f; p[2] = 1.5f + x - y; p[3] = 1.0f;
        }
    return px;
}

static void unitTests() {
    // Despill: neutral grey untouched, green spill removed, brightness kept.
    float r = 0.5f, g = 0.5f, b = 0.5f;
    ck2::despill(r, g, b, 1.0f, ck2::Screen::Green);
    CHECK(near(r, 0.5f) && near(g, 0.5f) && near(b, 0.5f));
    r = 0.2f; g = 0.8f; b = 0.2f;
    ck2::despill(r, g, b, 1.0f, ck2::Screen::Green);
    CHECK(near(g, 0.2f) && near(r, 0.5f) && near(b, 0.5f));
    CHECK(near(r + g + b, 1.2f));
    r = 0.2f; g = 0.8f; b = 0.2f;
    ck2::despill(r, g, b, 0.5f, ck2::Screen::Green);
    CHECK(near(g, 0.5f));
    r = 0.1f; g = 0.1f; b = 0.9f;
    ck2::despill(r, g, b, 1.0f, ck2::Screen::Blue);
    CHECK(near(b, 0.1f) && near(r, 0.5f) && near(g, 0.5f));

    // sRGB encode matches the standard curve.
    CHECK(near(ck2::srgbEncode(0.0f), 0.0f));
    CHECK(near(ck2::srgbEncode(1.0f), 1.0f));
    CHECK(near(ck2::srgbEncode(0.18f), 0.46135f, 1e-4f));

    // Compose.
    ck2::Look look;
    look.despill = 0.0f;
    float fg[3] = {0.18f, 0.18f, 0.18f}, src[4] = {0.3f, 0.4f, 0.5f, 1.0f}, out[4];
    ck2::composePixel(look, fg, 0.5f, src, out);
    CHECK(near(out[0], 0.46135f * 0.5f, 1e-4f) && near(out[3], 0.5f));
    look.output = ck2::Output::MatteOnly;
    ck2::composePixel(look, fg, 0.25f, src, out);
    CHECK(near(out[0], 0.25f) && near(out[3], 1.0f));
    look.output = ck2::Output::SourceWithMatte;
    ck2::composePixel(look, fg, 0.25f, src, out);
    CHECK(near(out[1], 0.4f) && near(out[3], 0.25f));
    look.output = ck2::Output::Foreground;
    look.input = ck2::InputSpace::SrgbLinear;
    ck2::composePixel(look, fg, 0.25f, src, out);
    CHECK(near(out[0], 0.18f) && near(out[3], 0.25f));
    // ACEScg: neutral grey stays grey (both whites map to each other), and the
    // matrices undo each other.
    look.input = ck2::InputSpace::ACEScg;
    ck2::composePixel(look, fg, 1.0f, src, out);
    CHECK(near(out[0], 0.18f, 1e-3f) && near(out[1], 0.18f, 1e-3f) && near(out[2], 0.18f, 1e-3f));
    float mr = 0.7f, mg = 0.2f, mb = 0.05f;
    ck2::applyMatrix(ck2::kAcescgToSrgb, mr, mg, mb);
    ck2::applyMatrix(ck2::kSrgbToAcescg, mr, mg, mb);
    CHECK(near(mr, 0.7f, 1e-4f) && near(mg, 0.2f, 1e-4f) && near(mb, 0.05f, 1e-4f));
    look.input = ck2::InputSpace::Display;

    // Log curves (1.02): 18% grey lands on each maker's published code value
    // (Canon: on Resolve's, measured 2026-11-16).
    {
        using C = ck2::Curve;
        struct G { C c; double grey; double tol; };
        const G greys[] = {{C::ACEScct, 0.4135884, 1e-5}, {C::DaVinciIntermediate, 0.336043, 1e-5},
                           {C::BmdFilmGen5, 0.383562, 1e-4}, {C::SLog3, 420.0 / 1023.0, 1e-6},
                           {C::LogC3, 0.391007, 1e-5}, {C::LogC4, 0.278396, 1e-5},
                           {C::VLog, 0.423311, 1e-5}, {C::CanonLog2, 0.38784, 2e-5},
                           {C::CanonLog3, 0.33096, 2e-5}, {C::RedLog3G10, 1.0 / 3.0, 1e-5},
                           {C::AppleLog, 0.488272, 1e-5}, {C::DjiDLog, 0.398765, 1e-5},
                           {C::DLogMPocket3, 0.400, 2e-3}, {C::DLogMAction5, 0.350, 2e-3}};
        for (const G& g : greys) {
            CHECK(std::fabs(ck2::encodeCurve(g.c, 0.18) - g.grey) < g.tol);
            // Encode and decode undo each other, from deep shadow to bright highlight
            // (and a little below black).
            for (double x : {-0.01, 0.0, 0.001, 0.005, 0.0105, 0.012, 0.05, 0.18, 1.0, 5.0, 40.0}) {
                const double back = ck2::decodeCurve(g.c, ck2::encodeCurve(g.c, x));
                CHECK(std::fabs(back - x) < 1e-6 * std::max(1.0, std::fabs(x)));
            }
            // No jump where the formula changes piece: small steps give small changes.
            for (double t = 0.0; t < 1.0; t += 0.0005) {
                const double d = std::fabs(ck2::decodeCurve(g.c, t + 0.0005) - ck2::decodeCurve(g.c, t));
                CHECK(d < 0.05 * std::max(1.0, ck2::decodeCurve(g.c, t)));
            }
        }
    }
    // Gamuts: white stays white (all D65), the two matrices undo each other,
    // and Rec.709 primaries themselves come out unchanged from a Rec.709 "gamut".
    for (int i = 0; i < ck2::kInputSpaceCount; ++i) {
        const auto s = static_cast<ck2::InputSpace>(i);
        const ck2::SpaceConversion& cv = ck2::conversion(s);
        float r = 1.0f, g = 1.0f, b = 1.0f;
        if (cv.matrix) ck2::applyMatrix(cv.toRec709, r, g, b);
        CHECK(near(r, 1.0f, 2e-3f) && near(g, 1.0f, 2e-3f) && near(b, 1.0f, 2e-3f));
        r = 0.6f; g = 0.3f; b = 0.1f;
        ck2::toModelSpace(cv, r, g, b);
        ck2::fromModelSpace(cv, r, g, b);
        CHECK(near(r, 0.6f, 1e-4f) && near(g, 0.3f, 1e-4f) && near(b, 0.1f, 1e-4f));
        CHECK(ck2::inputIsLinear(s) == (s != ck2::InputSpace::Display));
    }
    {
        double m[9], inv[9], id[9];
        ck2::rgbToXyz(ck2::kRec709, m);
        CHECK(std::fabs(m[3] - 0.212639) < 1e-5 && std::fabs(m[4] - 0.715169) < 1e-5);   // Rec.709 luma
        ck2::invert3(m, inv);
        ck2::mul3(m, inv, id);
        for (int k = 0; k < 9; ++k) CHECK(std::fabs(id[k] - (k % 4 == 0 ? 1.0 : 0.0)) < 1e-9);
        // S-Gamut3.Cine -> Rec.709, row 1 as published by Sony / colour-science.
        const ck2::SpaceConversion& sg = ck2::conversion(ck2::InputSpace::SLog3);
        CHECK(near(sg.toRec709[0], 1.6269474f, 2e-3f) && near(sg.toRec709[1], -0.5401385f, 2e-3f) &&
              near(sg.toRec709[2], -0.0868089f, 2e-3f));
    }
    // S-Log3 grey in, through the model's space and back: same code value out.
    look.input = ck2::InputSpace::SLog3;
    look.output = ck2::Output::Foreground;
    {
        float px[3] = {static_cast<float>(420.0 / 1023.0), static_cast<float>(420.0 / 1023.0),
                       static_cast<float>(420.0 / 1023.0)};
        ck2::toModelSpace(ck2::conversion(look.input), px[0], px[1], px[2]);
        CHECK(near(px[0], 0.18f, 1e-4f) && near(px[2], 0.18f, 1e-4f));
        ck2::composePixel(look, px, 1.0f, src, out);
        CHECK(near(out[0], 420.0f / 1023.0f, 1e-4f) && near(out[1], 420.0f / 1023.0f, 1e-4f));
    }
    look.input = ck2::InputSpace::Display;
    look.output = ck2::Output::Premultiplied;
    // DaVinci Wide Gamut Linear (1.03): no curve, DWG primaries; a DWG grey stays
    // grey, and pure DWG red is wider than Rec.709 (negative green and blue).
    {
        const ck2::SpaceConversion& d = ck2::conversion(ck2::InputSpace::DwgLinear);
        CHECK(d.curve == ck2::Curve::Linear && d.matrix);
        const ck2::SpaceConversion& di = ck2::conversion(ck2::InputSpace::DaVinciIntermediate);
        for (int k = 0; k < 9; ++k) CHECK(near(d.toRec709[k], di.toRec709[k], 1e-7f));
        float r = 0.18f, g = 0.18f, b = 0.18f;
        ck2::toModelSpace(d, r, g, b);
        CHECK(near(r, 0.18f, 1e-3f) && near(g, 0.18f, 1e-3f) && near(b, 0.18f, 1e-3f));
        r = 1.0f; g = 0.0f; b = 0.0f;
        ck2::toModelSpace(d, r, g, b);
        CHECK(r > 1.0f && g < 0.0f);
    }
    // 1.00's three spaces keep their cache names (Rec.709 = 0 + 10 in the hash).
    {
        uint32_t h = 2166136261u;
        for (int v : {10, 16, 0, 2, 0, 0, 0}) { h ^= static_cast<uint32_t>(v); h *= 16777619u; }
        CHECK(ck2::settingsHash(ck2::InputSpace::Display, 16, 0) == h);
    }
    // Menu order (1.06, Chadwick's): linear first, then the logs.
    CHECK(static_cast<int>(ck2::InputSpace::DwgLinear) == 3 &&
          static_cast<int>(ck2::InputSpace::DaVinciIntermediate) == 4 &&
          static_cast<int>(ck2::InputSpace::SLog3) == 5 &&
          static_cast<int>(ck2::InputSpace::BmdFilmGen5) == 6 &&
          static_cast<int>(ck2::InputSpace::RedLog3G10) == 10 &&
          static_cast<int>(ck2::InputSpace::VLog) == ck2::kInputSpaceCount - 1);
    // New spaces get their own cache names.
    for (int i = 0; i < ck2::kInputSpaceCount; ++i)
        for (int k = i + 1; k < ck2::kInputSpaceCount; ++k)
            CHECK(ck2::settingsHash(static_cast<ck2::InputSpace>(i), 16, 0) !=
                  ck2::settingsHash(static_cast<ck2::InputSpace>(k), 16, 0));

    // Names and JSON.
    CHECK(ck2::cacheFileName(12, 0xabcdef01u) == "f0000012_abcdef01.exr");
    using I = ck2::InputSpace;
    CHECK(ck2::settingsHash(I::Display, 0, 0) != ck2::settingsHash(I::SrgbLinear, 0, 0));
    CHECK(ck2::settingsHash(I::SrgbLinear, 0, 0) != ck2::settingsHash(I::ACEScg, 0, 0));
    CHECK(ck2::settingsHash(I::Display, 16, 0) != ck2::settingsHash(I::Display, 24, 0));
    using H = ck2::HintFrom;
    CHECK(ck2::settingsHash(I::Display, 0, 0, H::None, true, 0.5) == ck2::settingsHash(I::Display, 0, 0, H::None, false, 0.9));
    CHECK(ck2::settingsHash(I::Display, 0, 0, H::HintInput, true, 0.5) != ck2::settingsHash(I::Display, 0, 0, H::None));
    CHECK(ck2::settingsHash(I::Display, 0, 0, H::HintInput, true, 0.5) != ck2::settingsHash(I::Display, 0, 0, H::HintInput, false, 0.5));
    CHECK(ck2::settingsHash(I::Display, 0, 0, H::HintInput, true, 0.5) != ck2::settingsHash(I::Display, 0, 0, H::HintInput, true, 0.3));
    CHECK(ck2::settingsHash(I::Display, 0, 0, H::HintInput, true, 0.5) != ck2::settingsHash(I::Display, 0, 0, H::SourceAlpha, true, 0.5));
    std::string line = R"({"stage": "keying", "done": 12, "total": 84, "event": "progress", "t": 1.5})";
    CHECK(ck2::jsonField(line, "event") == "progress");
    CHECK(ck2::jsonField(line, "done") == "12");
    CHECK(ck2::jsonField(line, "t") == "1.5");
    const std::string errorLine = R"({"message": "a \"b\"", "event": "error"})";
    CHECK(ck2::jsonField(errorLine, "message") == "a \"b\"");

    // EXR round trip: DWAB 45 is lossy for colour (small error), exact for alpha.
    const int w = 96, h = 64;
    std::vector<float> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = 0.1f + 0.8f * x / w; p[1] = 0.2f + 0.6f * y / h; p[2] = 1.5f - x * 0.01f;
            p[3] = ((x / 8 + y / 8) % 3) / 2.0f;                       // a matte with hard steps
        }
    std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/ck2_rt.exr";
    std::string err;
    auto meanColourError = [&](const ck2::ImageRGBA& im) {
        double d = 0;
        for (size_t i = 0; i < px.size(); ++i) if (i % 4 != 3) d += std::fabs(im.pixels[i] - px[i]);
        return d / (px.size() * 3 / 4);
    };
    CHECK(ck2::writeExr(path, px.data(), w, h, w * 4, err));
    ck2::ImageRGBA img;
    CHECK(ck2::readExrRGBA(path, img, err));
    CHECK(img.width == w && img.height == h);
    CHECK(meanColourError(img) < 0.01);
    CHECK(img.pixels[3] == 1.0f);                                     // no alpha written: 1
    CHECK(ck2::writeExr(path, px.data(), w, h, w * 4, err, true));
    CHECK(ck2::readExrRGBA(path, img, err));
    bool alphaExact = true;
    for (size_t i = 3; i < px.size(); i += 4) alphaExact = alphaExact && img.pixels[i] == px[i];
    CHECK(alphaExact);
    CHECK(meanColourError(img) < 0.01);

    // Bottom-up source (negative stride) ends up top row first.
    std::vector<float> flipped(px.size());
    for (int y = 0; y < h; ++y)
        std::copy(&px[static_cast<size_t>(y) * w * 4], &px[static_cast<size_t>(y + 1) * w * 4],
                  &flipped[static_cast<size_t>(h - 1 - y) * w * 4]);
    CHECK(ck2::writeExr(path, &flipped[static_cast<size_t>(h - 1) * w * 4], w, h, -w * 4, err, true));
    CHECK(ck2::readExrRGBA(path, img, err));
    CHECK(meanColourError(img) < 0.01);
    std::remove(path.c_str());

    // Thumbnails: same picture ~0, a shifted picture clearly changed.
    std::vector<float> big(static_cast<size_t>(640) * 360 * 4);
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) {
            float* p = &big[(static_cast<size_t>(y) * 640 + x) * 4];
            p[0] = ((x / 40 + y / 40) % 2) ? 0.9f : 0.1f; p[1] = x / 640.0f; p[2] = 0.5f; p[3] = 1;
        }
    auto t1 = ck2::makeThumb(big.data(), 640, 360, 640 * 4);
    CHECK(ck2::thumbDiff(t1, t1) == 0.0);
    std::vector<float> shifted(big.size());
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x)
            for (int c = 0; c < 4; ++c)
                shifted[(static_cast<size_t>(y) * 640 + x) * 4 + c] = big[(static_cast<size_t>(y) * 640 + (x + 40) % 640) * 4 + c];
    CHECK(ck2::thumbDiff(t1, ck2::makeThumb(shifted.data(), 640, 360, 640 * 4)) > ck2::kThumbChanged);
    std::vector<float> noisy = big;
    for (size_t i = 0; i < noisy.size(); i += 4) noisy[i] += ((i / 4) % 7) * 0.002f;   // decode noise
    CHECK(ck2::thumbDiff(t1, ck2::makeThumb(noisy.data(), 640, 360, 640 * 4)) < ck2::kThumbChanged);
    // Threads (1.01): every row exactly once, and the threaded thumbnail
    // equals the single-thread one (odd size, bottom-up rows).
    for (int n : {1, 15, 16, 17, 359, 2160}) {
        std::vector<int> hits(n, 0);
        ck2::parallelBands(n, 16, [&](int a, int b) { for (int i = a; i < b; ++i) ++hits[i]; });
        CHECK(std::all_of(hits.begin(), hits.end(), [](int v) { return v == 1; }));
    }
    {
        const int w = 641, h = 359;
        std::vector<float> pic(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < pic.size(); ++i) pic[i] = static_cast<float>((i * 2654435761u) % 1000) / 999.0f;
        const float* top = &pic[static_cast<size_t>(h - 1) * w * 4];
        const long stride = -static_cast<long>(w) * 4;
        std::vector<double> acc(ck2::kThumbW * ck2::kThumbH * 3, 0.0);
        std::vector<int> cnt(ck2::kThumbW * ck2::kThumbH, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int t = (y * ck2::kThumbH / h) * ck2::kThumbW + x * ck2::kThumbW / w;
                for (int c = 0; c < 3; ++c) acc[t * 3 + c] += top[stride * y + x * 4 + c];
                ++cnt[t];
            }
        std::vector<float> ref(acc.size());
        for (size_t i = 0; i < ref.size(); ++i) ref[i] = static_cast<float>(acc[i] / cnt[i / 3]);
        CHECK(ck2::makeThumb(top, w, h, stride) == ref);
    }

    std::string tp = path + ".thumb";
    std::vector<float> back;
    CHECK(ck2::writeThumb(tp, t1) && ck2::readThumb(tp, back) && back == t1);
    std::remove(tp.c_str());

    // Range.
    long f0, f1;
    CHECK(ck2::processRange(0, 99, 0, 99, false, 5, 10, f0, f1) && f0 == 0 && f1 == 99);
    CHECK(ck2::processRange(0, 99, 0, 99, true, 5, 10, f0, f1) && f0 == 5 && f1 == 10);
    CHECK(ck2::processRange(0, 99, 0, 99, true, -3, 200, f0, f1) && f0 == 0 && f1 == 99);
    CHECK(!ck2::processRange(0, 99, 0, 99, true, 50, 40, f0, f1));
    // Timeline bounds past the clip never reach beyond the clip's frames.
    CHECK(ck2::processRange(1001, 1023, 0, 86400, false, 0, 0, f0, f1) && f0 == 1001 && f1 == 1023);
    CHECK(ck2::processRange(1001, 1023, 1010, 1100, false, 0, 0, f0, f1) && f0 == 1010 && f1 == 1023);
    CHECK(ck2::processRange(0, 99, 10, 50, false, 0, 0, f0, f1) && f0 == 10 && f1 == 51);   // one frame of slack
    // Mixed frame rates: Resolve reported the timeline end one short (sh280).
    CHECK(ck2::processRange(1001, 1094.9999, 1001, 1093, false, 0, 0, f0, f1) && f0 == 1001 && f1 == 1094);
    // ... but never past the clip.
    CHECK(ck2::processRange(1001, 1023.9999, 1001, 1023, false, 0, 0, f0, f1) && f1 == 1023);
    // Fusion's fractional clip end: [1001, 1023.9999] is frames 1001 to 1023.
    CHECK(ck2::processRange(1001, 1023.9999, 0, 2000, false, 0, 0, f0, f1) && f0 == 1001 && f1 == 1023);
    CHECK(ck2::processRange(0, 22, 0, 22, false, 0, 0, f0, f1) && f0 == 0 && f1 == 22);
    // No timeline bounds (0, 0): the clip's frames.
    CHECK(ck2::processRange(1001, 1023, 0, 0, false, 0, 0, f0, f1) && f0 == 1001 && f1 == 1023);
    CHECK(!ck2::processRange(1001, 1023, 0, 500, false, 0, 0, f0, f1));
}

// Runs a real job through ck2::Job, as the plugin does, and prints each update.
static int runJob(const char* python, const char* worker, const char* job, const char* log) {
    std::mutex m;
    std::condition_variable cv;
    bool finished = false, ok = false;
    std::string err, last;
    auto j = ck2::Job::start(python, worker, job, log, [&](const ck2::JobUpdate& u) {
        if (u.status != last) { std::printf("  %s\n", u.status.c_str()); last = u.status; }
        if (u.finished) { std::lock_guard<std::mutex> l(m); finished = true; ok = u.ok; cv.notify_all(); }
    }, err);
    if (!j) { std::printf("start failed: %s\n", err.c_str()); return 1; }
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return finished; });
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "jobjson") {      // for the Python check in build.sh
        ck2::JobSpec j;
        j.modelDir = "/m \"q\""; j.framesDir = "/f"; j.workDir = "/w"; j.outputDir = "/o";
        j.hintDir = "/h"; j.window = 24; j.precision = "fp16"; j.inputLinear = true;
        j.hintFrom = ck2::HintFrom::HintInput; j.hintEveryFrame = false; j.hintStrength = 0.3;
        j.outputNames = {"a.exr", "b.exr"};
        std::fputs(ck2::jobJson(j).c_str(), stdout);
        return 0;
    }
    if (argc == 6 && std::string(argv[1]) == "job") return runJob(argv[2], argv[3], argv[4], argv[5]);
    // For tests/colour_check.py: "r g b" lines on stdin, converted with the
    // plugin's own code. encode: linear Rec.709 -> Input Is space <n> (what the
    // output does); decode: the other way (what Process does to the source).
    if (argc == 4 && std::string(argv[1]) == "convert") {
        const int n = std::atoi(argv[2]);
        if (n < 0 || n >= ck2::kInputSpaceCount) return 2;
        const bool encode = std::string(argv[3]) == "encode";
        const ck2::SpaceConversion& cv = ck2::conversion(static_cast<ck2::InputSpace>(n));
        float r, g, b;
        while (std::scanf("%f %f %f", &r, &g, &b) == 3) {
            if (encode) ck2::fromModelSpace(cv, r, g, b);
            else ck2::toModelSpace(cv, r, g, b);
            std::printf("%.9g %.9g %.9g\n", r, g, b);
        }
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "write") {
        auto px = pattern(7, 5);
        std::string err;
        if (!ck2::writeExr(argv[2], px.data(), 7, 5, 7 * 4, err)) { std::puts(err.c_str()); return 1; }
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "read") {
        ck2::ImageRGBA img;
        std::string err;
        if (!ck2::readExrRGBA(argv[2], img, err)) { std::puts(err.c_str()); return 1; }
        std::printf("%d %d\n", img.width, img.height);
        for (int i : {0, img.width * img.height / 2, img.width * img.height - 1})
            std::printf("%d %.5f %.5f %.5f %.5f\n", i, img.pixels[i * 4], img.pixels[i * 4 + 1],
                        img.pixels[i * 4 + 2], img.pixels[i * 4 + 3]);
        return 0;
    }
    unitTests();
    std::printf(failures ? "%d test(s) FAILED\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
