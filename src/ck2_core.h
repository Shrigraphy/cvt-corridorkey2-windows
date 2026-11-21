// ---------------------------------------------------------------------------
// CorridorKey 2 for Resolve - host-free helpers (pixels, names, job events)
//
// From Creative Video Tips (CVT)  |  https://creativevideotips.com
// Licence: CC BY-NC-SA 4.0 with the CorridorKey additional terms (see LICENSE).
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <filesystem>
#endif
#include <locale>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ck2 {

constexpr const char* kVersion = "1.06";
constexpr int kVersionMajor = 1;        // OFX major stays 1 so saved comps keep the node
constexpr int kVersionMinor = 106;      // 0.30 -> 30, 1.00 -> 100, 1.06 -> 106

enum class Output { Premultiplied = 0, MatteOnly = 1, Foreground = 2, SourceWithMatte = 3 };

// What the picture coming in is. CorridorKey works in linear light with
// sRGB/Rec.709 primaries. Every other space is converted on the way in (curve
// to linear, then its primaries to Rec.709) and back on the way out, so the
// output stays in the input's space. The numbers are saved in comps and are
// the menu order (Chadwick's, 1.06): 0-2 are fixed since 1.00; 3 and up were
// reordered in 1.04 and 1.06 (never public before), so only add at the end now.
enum class InputSpace {
    Display = 0, SrgbLinear = 1, ACEScg = 2,
    DwgLinear = 3,             // DaVinci Wide Gamut, linear: what Fusion gets in a DaVinci colour-managed project
    DaVinciIntermediate = 4,   // DaVinci Wide Gamut / DaVinci Intermediate
    SLog3 = 5, BmdFilmGen5 = 6, ACEScct = 7, LogC3 = 8, LogC4 = 9, RedLog3G10 = 10,
    CanonLog2 = 11, CanonLog3 = 12, AppleLog = 13, AppleLog2 = 14,
    DLogMPocket3 = 15, DLogMAction5 = 16, DjiDLog = 17, VLog = 18
};
constexpr int kInputSpaceCount = 19;

// ACEScg (AP1, D60) <-> linear sRGB/Rec.709 (D65) with CAT02 adaptation, as
// Resolve's Color Space Transform does it (measured 2026-11-16, within 5e-7;
// 1.00-1.04 used Bradford, about 0.003 off).
constexpr float kAcescgToSrgb[9] = {1.7050796436f, -0.6242334615f, -0.0808461821f,
                                    -0.1297005301f, 1.1384685466f, -0.0087680165f,
                                    -0.0241663449f, -0.1246141581f, 1.1487805030f};
constexpr float kSrgbToAcescg[9] = {0.6130828922f, 0.3411670542f, 0.0457500535f,
                                    0.0700035498f, 0.9180628304f, 0.0119336198f,
                                    0.0204907778f, 0.1067639876f, 0.8727452346f};

inline void applyMatrix(const float* m, float& r, float& g, float& b) {
    const float r2 = m[0] * r + m[1] * g + m[2] * b;
    const float g2 = m[3] * r + m[4] * g + m[5] * b;
    const float b2 = m[6] * r + m[7] * g + m[8] * b;
    r = r2; g = g2; b = b2;
}

// ---- Camera log curves: code value (0-1, full range) <-> scene linear (0.18 grey)
// From the makers' published formulas, as Resolve's Color Space Transform uses
// them (checked against Resolve 2026-11-16). Canon: Resolve takes scene linear
// as it is, without Canon's 0.9 reflection scaling.
enum class Curve { Linear, ACEScct, DaVinciIntermediate, BmdFilmGen5, SLog3, LogC3, LogC4, VLog,
                   CanonLog2, CanonLog3, RedLog3G10, AppleLog, DjiDLog, DLogMPocket3, DLogMAction5 };

namespace curve_detail {
constexpr double kLogC4A = (262144.0 - 16.0) / 117.45, kLogC4B = (1023.0 - 95.0) / 1023.0,
                 kLogC4C = 95.0 / 1023.0;
inline double logC4S() { return 7.0 * std::log(2.0) * std::pow(2.0, 7.0 - 14.0 * kLogC4C / kLogC4B) / (kLogC4A * kLogC4B); }
inline double logC4T() { return (std::pow(2.0, 14.0 * (-kLogC4C / kLogC4B) + 6.0) - 64.0) / kLogC4A; }
inline double legalToFull(double v) { return (v * 1023.0 - 64.0) / 876.0; }   // Canon: code value -> IRE
inline double fullToLegal(double v) { return (v * 876.0 + 64.0) / 1023.0; }

// Apple Log and Apple Log 2 (Apple's white papers): a log curve with a squared toe.
constexpr double kAppleR0 = -0.05641088, kAppleRt = 0.01, kAppleC = 47.28711236,
                 kAppleB = 0.00964052, kAppleG = 0.08550479, kAppleD = 0.69336945;

// DJI D-Log M has no published formula. These fits to real footage are by
// Thatcher Freeman (github.com/thatcherfreeman/dwg-transforms), used with his
// permission: lin = gain * f(2^(x * scale + yShift) + xShift), where f is
// slope * v + intercept below cut and slope2 * v above.
struct DLogMFit { double xShift, yShift, scale, slope, slope2, intercept, cut, gain; };
constexpr DLogMFit kDLogMPocket3{-2.428226947784424, 0.9327186346054077, 5.612990379333496,
                                 1.0151796340942383, 1.8734303712844849, 0.5178895592689514,
                                 0.6034245491027832, 0.18 / 12.4054};
constexpr DLogMFit kDLogMAction5{-2.426687717437744, 0.9350103139877319, 6.127270221710205,
                                 1.032887578010559, 1.8330339193344116, 0.5343853235244751,
                                 0.6678594946861267, 0.016293399035930634};
inline double dlogmDecode(const DLogMFit& f, double t) {
    const double v = std::pow(2.0, t * f.scale + f.yShift) + f.xShift;
    return f.gain * (v < f.cut ? v * f.slope + f.intercept : v * f.slope2);
}
inline double dlogmEncode(const DLogMFit& f, double x) {
    const double y = x / f.gain;
    const double v = y < f.slope2 * f.cut ? (y - f.intercept) / f.slope : y / f.slope2;
    return (std::log2(std::max(v - f.xShift, 1e-9)) - f.yShift) / f.scale;
}
}  // namespace curve_detail

inline double decodeCurve(Curve c, double t) {
    using namespace curve_detail;
    switch (c) {
        case Curve::Linear: return t;
        case Curve::ACEScct:
            if (t <= 0.155251141552511) return (t - 0.0729055341958355) / 10.5402377416545;
            return std::pow(2.0, t * 17.52 - 9.72);
        case Curve::DaVinciIntermediate:
            if (t <= 0.02740668) return t / 10.44426855;
            return std::pow(2.0, t / 0.07329248 - 7.0) - 0.0075;
        case Curve::BmdFilmGen5: {
            const double A = 0.08692876065491224, B = 0.005494072432257808, C = 0.5300133392291939,
                         D = 8.283605932402494, E = 0.09246575342465753;
            if (t < D * 0.005 + E) return (t - E) / D;
            return std::exp((t - C) / A) - B;
        }
        case Curve::SLog3:
            if (t >= 171.2102946929 / 1023.0) return std::pow(10.0, (t * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01;
            return (t * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
        case Curve::LogC3: {   // EI 800
            const double cut = 0.010591, a = 5.555556, b = 0.052272, cc = 0.247190, d = 0.385537,
                         e = 5.367655, f = 0.092809;
            if (t > e * cut + f) return (std::pow(10.0, (t - d) / cc) - b) / a;
            return (t - f) / e;
        }
        case Curve::LogC4:
            if (t >= 0.0) return (std::pow(2.0, 14.0 * (t - kLogC4C) / kLogC4B + 6.0) - 64.0) / kLogC4A;
            return t * logC4S() + logC4T();
        case Curve::VLog:
            if (t < 0.181) return (t - 0.125) / 5.6;
            return std::pow(10.0, (t - 0.598206) / 0.241514) - 0.00873;
        case Curve::CanonLog2: {   // v1.2: constants already in code values (no legal step)
            const double v = t;
            return v < 0.092864125 ? -(std::pow(10.0, (0.092864125 - v) / 0.24136077) - 1.0) / 87.09937546
                                   : (std::pow(10.0, (v - 0.092864125) / 0.24136077) - 1.0) / 87.09937546;
        }
        case Curve::CanonLog3: {
            const double v = legalToFull(t);
            double x;
            if (v < 0.04076162) x = -(std::pow(10.0, (0.07623209 - v) / 0.42889912) - 1.0) / 14.98325;
            else if (v <= 0.105357102) x = (v - 0.073059361) / 2.3069815;
            else x = (std::pow(10.0, (v - 0.069886632) / 0.42889912) - 1.0) / 14.98325;
            return x;
        }
        case Curve::RedLog3G10:
            if (t < 0.0) return t / 15.1927 - 0.01;
            return (std::pow(10.0, t / 0.224282) - 1.0) / 155.975327 - 0.01;
        case Curve::AppleLog:
            if (t >= kAppleC * (kAppleRt - kAppleR0) * (kAppleRt - kAppleR0))
                return std::pow(2.0, (t - kAppleD) / kAppleG) - kAppleB;
            if (t > 0.0) return std::sqrt(t / kAppleC) + kAppleR0;
            return kAppleR0;
        case Curve::DjiDLog:
            if (t <= 0.14) return (t - 0.0929) / 6.025;
            return (std::pow(10.0, 3.89616 * t - 2.27752) - 0.0108) / 0.9892;
        case Curve::DLogMPocket3: return dlogmDecode(kDLogMPocket3, t);
        case Curve::DLogMAction5: return dlogmDecode(kDLogMAction5, t);
    }
    return t;
}

inline double encodeCurve(Curve c, double x) {
    using namespace curve_detail;
    switch (c) {
        case Curve::Linear: return x;
        case Curve::ACEScct:
            if (x <= 0.0078125) return 10.5402377416545 * x + 0.0729055341958355;
            return (std::log2(x) + 9.72) / 17.52;
        case Curve::DaVinciIntermediate:
            if (x <= 0.00262409) return x * 10.44426855;
            return (std::log2(x + 0.0075) + 7.0) * 0.07329248;
        case Curve::BmdFilmGen5: {
            const double A = 0.08692876065491224, B = 0.005494072432257808, C = 0.5300133392291939,
                         D = 8.283605932402494, E = 0.09246575342465753;
            if (x < 0.005) return D * x + E;
            return A * std::log(x + B) + C;
        }
        case Curve::SLog3:
            if (x >= 0.01125) return (420.0 + std::log10((x + 0.01) / 0.19) * 261.5) / 1023.0;
            return (x * (171.2102946929 - 95.0) / 0.01125 + 95.0) / 1023.0;
        case Curve::LogC3: {
            const double cut = 0.010591, a = 5.555556, b = 0.052272, cc = 0.247190, d = 0.385537,
                         e = 5.367655, f = 0.092809;
            if (x > cut) return cc * std::log10(a * x + b) + d;
            return e * x + f;
        }
        case Curve::LogC4:
            if (x >= logC4T()) return (std::log2(kLogC4A * x + 64.0) - 6.0) / 14.0 * kLogC4B + kLogC4C;
            return (x - logC4T()) / logC4S();
        case Curve::VLog:
            if (x < 0.01) return 5.6 * x + 0.125;
            return 0.241514 * std::log10(x + 0.00873) + 0.598206;
        case Curve::CanonLog2: {
            const double v = x;
            const double y = v < 0.0 ? -(0.24136077 * std::log10(-v * 87.09937546 + 1.0) - 0.092864125)
                                     : 0.24136077 * std::log10(v * 87.09937546 + 1.0) + 0.092864125;
            return y;
        }
        case Curve::CanonLog3: {
            const double v = x;
            double y;
            if (v < -0.014) y = -0.42889912 * std::log10(-v * 14.98325 + 1.0) + 0.07623209;
            else if (v <= 0.014) y = 2.3069815 * v + 0.073059361;
            else y = 0.42889912 * std::log10(v * 14.98325 + 1.0) + 0.069886632;
            return fullToLegal(y);
        }
        case Curve::RedLog3G10: {
            const double v = x + 0.01;
            if (v < 0.0) return v * 15.1927;
            return 0.224282 * std::log10(v * 155.975327 + 1.0);
        }
        case Curve::AppleLog:
            if (x >= kAppleRt) return kAppleG * std::log2(x + kAppleB) + kAppleD;
            if (x >= kAppleR0) return kAppleC * (x - kAppleR0) * (x - kAppleR0);
            return 0.0;
        case Curve::DjiDLog:
            if (x <= 0.0078) return 6.025 * x + 0.0929;
            return std::log10(x * 0.9892 + 0.0108) * 0.256663 + 0.584555;
        case Curve::DLogMPocket3: return dlogmEncode(kDLogMPocket3, x);
        case Curve::DLogMAction5: return dlogmEncode(kDLogMAction5, x);
    }
    return x;
}

// ---- Camera gamuts -> Rec.709 primaries (all D65 white, so no adaptation)
struct Primaries { double rx, ry, gx, gy, bx, by, wx = 0.3127, wy = 0.3290; };   // white: D65 unless given
constexpr Primaries kRec709{0.64, 0.33, 0.30, 0.60, 0.15, 0.06};

inline void invert3(const double* m, double* o) {
    const double a = m[4] * m[8] - m[5] * m[7], b = m[5] * m[6] - m[3] * m[8], c = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * a + m[1] * b + m[2] * c;
    o[0] = a / det; o[1] = (m[2] * m[7] - m[1] * m[8]) / det; o[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    o[3] = b / det; o[4] = (m[0] * m[8] - m[2] * m[6]) / det; o[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    o[6] = c / det; o[7] = (m[1] * m[6] - m[0] * m[7]) / det; o[8] = (m[0] * m[4] - m[1] * m[3]) / det;
}
// RGB -> XYZ for these primaries and their white.
inline void rgbToXyz(const Primaries& p, double* m) {
    const double P[9] = {p.rx / p.ry, p.gx / p.gy, p.bx / p.by, 1, 1, 1,
                         (1 - p.rx - p.ry) / p.ry, (1 - p.gx - p.gy) / p.gy, (1 - p.bx - p.by) / p.by};
    double inv[9];
    invert3(P, inv);
    const double W[3] = {p.wx / p.wy, 1.0, (1 - p.wx - p.wy) / p.wy};
    for (int col = 0; col < 3; ++col) {
        const double s = inv[col * 3] * W[0] + inv[col * 3 + 1] * W[1] + inv[col * 3 + 2] * W[2];
        for (int row = 0; row < 3; ++row) m[row * 3 + col] = P[row * 3 + col] * s;
    }
}
inline void mul3(const double* a, const double* b, double* o) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

// How each InputSpace is converted for the model.
struct SpaceConversion {
    Curve curve = Curve::Linear;
    bool matrix = false;
    float toRec709[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float fromRec709[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

inline SpaceConversion makeConversion(InputSpace s) {
    SpaceConversion c;
    auto gamut = [&c](const Primaries& p) {
        double toXyz[9], rec[9], recInv[9], to[9], toInv[9];
        rgbToXyz(p, toXyz);
        rgbToXyz(kRec709, rec);
        invert3(rec, recInv);
        mul3(recInv, toXyz, to);
        invert3(to, toInv);
        for (int i = 0; i < 9; ++i) { c.toRec709[i] = static_cast<float>(to[i]); c.fromRec709[i] = static_cast<float>(toInv[i]); }
        c.matrix = true;
    };
    // A camera-native -> DaVinci Wide Gamut matrix (Thatcher Freeman's D-Log M fits).
    auto viaDwg = [&c](const double* toDwg) {
        double dwg[9], rec[9], recInv[9], dwgTo709[9], to[9], toInv[9];
        rgbToXyz({0.8000, 0.3130, 0.1682, 0.9877, 0.0790, -0.1155}, dwg);
        rgbToXyz(kRec709, rec);
        invert3(rec, recInv);
        mul3(recInv, dwg, dwgTo709);
        mul3(dwgTo709, toDwg, to);
        invert3(to, toInv);
        for (int i = 0; i < 9; ++i) { c.toRec709[i] = static_cast<float>(to[i]); c.fromRec709[i] = static_cast<float>(toInv[i]); }
        c.matrix = true;
    };
    auto aces = [&c] {
        std::copy(kAcescgToSrgb, kAcescgToSrgb + 9, c.toRec709);
        std::copy(kSrgbToAcescg, kSrgbToAcescg + 9, c.fromRec709);
        c.matrix = true;
    };
    switch (s) {
        case InputSpace::Display: case InputSpace::SrgbLinear: break;
        case InputSpace::ACEScg: aces(); break;
        case InputSpace::ACEScct: c.curve = Curve::ACEScct; aces(); break;
        case InputSpace::DaVinciIntermediate:   // DaVinci Wide Gamut
            c.curve = Curve::DaVinciIntermediate; gamut({0.8000, 0.3130, 0.1682, 0.9877, 0.0790, -0.1155}); break;
        case InputSpace::BmdFilmGen5:           // Blackmagic Wide Gamut Gen 4/5
            c.curve = Curve::BmdFilmGen5;
            gamut({0.7177215, 0.3171181, 0.2280410, 0.8615690, 0.1005841, -0.0820452, 0.3127170, 0.3290312});
            break;
        case InputSpace::SLog3:                 // S-Gamut3.Cine
            c.curve = Curve::SLog3; gamut({0.766, 0.275, 0.225, 0.800, 0.089, -0.087}); break;
        case InputSpace::LogC3:                 // ARRI Wide Gamut 3
            c.curve = Curve::LogC3; gamut({0.684, 0.313, 0.221, 0.848, 0.0861, -0.102}); break;
        case InputSpace::LogC4:                 // ARRI Wide Gamut 4
            c.curve = Curve::LogC4; gamut({0.7347, 0.2653, 0.1424, 0.8576, 0.0991, -0.0308}); break;
        case InputSpace::VLog:                  // V-Gamut
            c.curve = Curve::VLog; gamut({0.730, 0.280, 0.165, 0.840, 0.100, -0.030}); break;
        case InputSpace::CanonLog2:             // Cinema Gamut
            c.curve = Curve::CanonLog2; gamut({0.740, 0.270, 0.170, 1.140, 0.080, -0.100}); break;
        case InputSpace::CanonLog3:
            c.curve = Curve::CanonLog3; gamut({0.740, 0.270, 0.170, 1.140, 0.080, -0.100}); break;
        case InputSpace::DwgLinear:
            gamut({0.8000, 0.3130, 0.1682, 0.9877, 0.0790, -0.1155}); break;
        case InputSpace::AppleLog:              // Rec.2020
            c.curve = Curve::AppleLog; gamut({0.708, 0.292, 0.170, 0.797, 0.131, 0.046}); break;
        case InputSpace::AppleLog2:             // Apple Gamut (Apple Log 2 white paper)
            c.curve = Curve::AppleLog; gamut({0.725, 0.301, 0.221, 0.814, 0.068, -0.076}); break;
        case InputSpace::DjiDLog:               // DJI D-Gamut
            c.curve = Curve::DjiDLog; gamut({0.71, 0.31, 0.21, 0.88, 0.09, -0.08}); break;
        case InputSpace::DLogMPocket3: {
            static const double m[9] = {0.69848738, 0.19665509, 0.10485752, -0.00579783, 1.00886749,
                                        -0.00306966, 0.03929425, 0.20805949, 0.75264626};
            c.curve = Curve::DLogMPocket3; viaDwg(m); break;
        }
        case InputSpace::DLogMAction5: {
            static const double m[9] = {0.79846643, 0.07372841, 0.12780516, 0.06153171, 0.7303191,
                                        0.20814919, 0.05980929, -0.04343518, 0.98362589};
            c.curve = Curve::DLogMAction5; viaDwg(m); break;
        }
        case InputSpace::RedLog3G10:            // REDWideGamutRGB
            c.curve = Curve::RedLog3G10; gamut({0.780308, 0.304253, 0.121595, 1.493994, 0.095612, -0.084589}); break;
    }
    return c;
}

// Made once per space (thread-safe), so render can use it on every pixel.
inline const SpaceConversion& conversion(InputSpace s) {
    static const std::vector<SpaceConversion> all = [] {
        std::vector<SpaceConversion> v;
        for (int i = 0; i < kInputSpaceCount; ++i) v.push_back(makeConversion(static_cast<InputSpace>(i)));
        return v;
    }();
    const int i = static_cast<int>(s);
    return all[(i >= 0 && i < kInputSpaceCount) ? i : 0];
}

// True when the model is given linear light (everything but Rec.709 display pictures).
inline bool inputIsLinear(InputSpace s) { return s != InputSpace::Display; }

// Input picture -> linear Rec.709 for the model (Display and sRGB Linear: unchanged).
inline void toModelSpace(const SpaceConversion& c, float& r, float& g, float& b) {
    if (c.curve != Curve::Linear) {
        r = static_cast<float>(decodeCurve(c.curve, r));
        g = static_cast<float>(decodeCurve(c.curve, g));
        b = static_cast<float>(decodeCurve(c.curve, b));
    }
    if (c.matrix) applyMatrix(c.toRec709, r, g, b);
}
// Linear Rec.709 -> the input's space (not for Display, which uses srgbEncode).
inline void fromModelSpace(const SpaceConversion& c, float& r, float& g, float& b) {
    if (c.matrix) applyMatrix(c.fromRec709, r, g, b);
    if (c.curve != Curve::Linear) {
        r = static_cast<float>(encodeCurve(c.curve, r));
        g = static_cast<float>(encodeCurve(c.curve, g));
        b = static_cast<float>(encodeCurve(c.curve, b));
    }
}

enum class Screen { Green = 0, Blue = 1 };

inline float srgbEncode(float v) {
    if (v <= 0.0031308f) return 12.92f * v;
    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// Takes the screen colour's excess over the average of the other two channels
// and shares it out to them, so brightness stays about the same.
inline void despill(float& r, float& g, float& b, float strength, Screen screen) {
    if (strength <= 0.0f) return;
    float& s = (screen == Screen::Green) ? g : b;
    float& o1 = r;
    float& o2 = (screen == Screen::Green) ? b : g;
    float excess = s - 0.5f * (o1 + o2);
    if (excess <= 0.0f) return;
    excess *= strength;
    s -= excess;
    o1 += 0.5f * excess;
    o2 += 0.5f * excess;
}

struct Look {
    Output output = Output::Premultiplied;
    float despill = 0.5f;
    Screen screen = Screen::Green;
    InputSpace input = InputSpace::Display;
};

// One output pixel from the cached foreground (linear, straight), the matte
// and the source pixel (as the host gave it).
inline void composePixel(const Look& look, const float* fgLinear, float alpha,
                         const float* src, float* dst) {
    float a = std::fmin(std::fmax(alpha, 0.0f), 1.0f);
    switch (look.output) {
        case Output::MatteOnly:
            dst[0] = dst[1] = dst[2] = a; dst[3] = 1.0f;
            return;
        case Output::SourceWithMatte:
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = a;
            return;
        default: break;
    }
    float r = fgLinear[0], g = fgLinear[1], b = fgLinear[2];
    despill(r, g, b, look.despill, look.screen);
    if (look.input == InputSpace::Display) {
        r = srgbEncode(std::fmax(r, 0.0f));
        g = srgbEncode(std::fmax(g, 0.0f));
        b = srgbEncode(std::fmax(b, 0.0f));
    } else {
        fromModelSpace(conversion(look.input), r, g, b);
    }
    if (look.output == Output::Premultiplied) { r *= a; g *= a; b *= a; }
    dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
}

enum class HintFrom { None = 0, HintInput = 1, SourceAlpha = 2 };


// Settings that change the matte itself (not the look): part of the cache name.
// hintFrom is what was actually used (None when the Hint input is empty).
inline uint32_t settingsHash(InputSpace input, int window, int precision,
                             HintFrom hintFrom = HintFrom::None, bool hintEveryFrame = true,
                             double hintStrength = 0.5) {
    uint32_t h = 2166136261u;
    const int hs = hintFrom == HintFrom::None ? 0 : static_cast<int>(std::lround(hintStrength * 1000));
    const int hf = hintFrom == HintFrom::None ? 0 : (hintEveryFrame ? 1 : 2);
    // 0-2 keep their 1.00 names; 3 and up use + 100 so results made with the
    // 1.02-1.03 menu order are never read for a different space.
    // 0-1 keep their 1.00 key; ACEScg changed in 1.05 (CAT02, as Resolve). 3 and
    // up use + 200 since the 1.06 menu order, so results made under an older
    // order (1.02-1.05 used + 10, + 100, + 1100) are never read for another space.
    const int in = static_cast<int>(input);
    const int key = in <= 1 ? in + 10 : in == 2 ? 1012 : in + 200;
    for (int v : {key, window, precision, 2 /* cache format: DWAB */,
                  static_cast<int>(hintFrom), hf, hs}) {
        h ^= static_cast<uint32_t>(v);
        h *= 16777619u;
    }
    return h;
}

inline std::string cacheFileName(long frame, uint32_t settings) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "f%07ld_%08x.exr", frame, settings);
    return buf;
}

inline std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, 8, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

// Everything the worker needs for one Process (see load_job() in the worker).
struct JobSpec {
    std::string modelDir, framesDir, workDir, outputDir, hintDir;
    bool inputLinear = false;
    int window = 0;                         // 0 = auto
    std::string precision = "auto";
    HintFrom hintFrom = HintFrom::None;
    bool hintEveryFrame = true;
    double hintStrength = 0.5;
    std::vector<std::string> outputNames;
    bool privateFiles = true;               // results readable only by this user
};

inline std::string jobJson(const JobSpec& j) {
    std::ostringstream js;
    js.imbue(std::locale::classic());        // always 0.5, never 0,5
    js << "{\n  \"model_dir\": " << jsonString(j.modelDir)
       << ",\n  \"frames_dir\": " << jsonString(j.framesDir)
       << ",\n  \"work_dir\": " << jsonString(j.workDir)
       << ",\n  \"output_dir\": " << jsonString(j.outputDir)
       << ",\n  \"input_linear\": " << (j.inputLinear ? "true" : "false")
       << ",\n  \"window\": " << (j.window == 0 ? std::string("\"auto\"") : std::to_string(j.window))
       << ",\n  \"precision\": " << jsonString(j.precision)
       << ",\n  \"hint\": {\"mode\": "
       << (j.hintFrom == HintFrom::None ? "\"none\"" : j.hintEveryFrame ? "\"every\"" : "\"first\"")
       << ", \"dir\": " << jsonString(j.hintDir)
       << ", \"strength\": " << j.hintStrength << ", \"carry\": true}"
       << ",\n  \"private\": " << (j.privateFiles ? "true" : "false")
       << ",\n  \"output_names\": [";
    for (size_t i = 0; i < j.outputNames.size(); ++i)
        js << (i ? ", " : "") << jsonString(j.outputNames[i]);
    js << "]\n}\n";
    return js.str();
}

// --- Threads ------------------------------------------------------------------

// How many threads the per-frame work uses (all performance cores, at most 16).
inline int workerCount() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 4 : static_cast<int>(std::min(n, 16u));
}

// Runs fn(begin, end) on contiguous bands of [0, n), in parallel, each band at
// least minPerBand long. fn must not throw. Returns when every band is done.
template <class Fn>
inline void parallelBands(int n, int minPerBand, Fn&& fn) {
    if (n <= 0) return;
    const int bands = std::max(1, std::min(workerCount(), n / std::max(1, minPerBand)));
    if (bands == 1) { fn(0, n); return; }
    std::vector<std::thread> threads;
    threads.reserve(bands - 1);
    for (int b = 1; b < bands; ++b)
        threads.emplace_back([&fn, n, b, bands] {
            fn(static_cast<int>(static_cast<long>(n) * b / bands),
               static_cast<int>(static_cast<long>(n) * (b + 1) / bands));
        });
    fn(0, static_cast<int>(static_cast<long>(n) / bands));
    for (auto& t : threads) t.join();
}

// --- Thumbnails: tell whether the picture still matches the stored matte ---

constexpr int kThumbW = 64, kThumbH = 36;

// Box-averaged RGB thumbnail of an RGBA float picture (top row first, rows
// `rowStride` floats apart; negative for bottom-up). Values clamped to 0..1.
inline std::vector<float> makeThumb(const float* top, int w, int h, long rowStride) {
    std::vector<double> acc(kThumbW * kThumbH * 3, 0.0);
    std::vector<int> cnt(kThumbW * kThumbH, 0);
    // One band of thumbnail rows per thread: each cell is summed by one thread
    // in the same order as before, so the result matches the single-thread one.
    parallelBands(kThumbH, 1, [&](int ty0, int ty1) {
        if (h <= 0) return;
        const int y0 = static_cast<int>((static_cast<long>(ty0) * h + kThumbH - 1) / kThumbH);
        const int y1 = static_cast<int>((static_cast<long>(ty1) * h + kThumbH - 1) / kThumbH);
        for (int y = y0; y < y1; ++y) {
            const float* row = top + rowStride * y;
            const int ty = y * kThumbH / h;
            for (int x = 0; x < w; ++x) {
                const int t = ty * kThumbW + x * kThumbW / w;
                for (int c = 0; c < 3; ++c) {
                    float v = row[x * 4 + c];
                    acc[t * 3 + c] += v < 0 ? 0 : (v > 1 ? 1 : v);
                }
                ++cnt[t];
            }
        }
    });
    std::vector<float> out(acc.size(), 0.0f);
    for (int t = 0; t < kThumbW * kThumbH; ++t)
        for (int c = 0; c < 3; ++c)
            out[t * 3 + c] = cnt[t] ? static_cast<float>(acc[t * 3 + c] / cnt[t]) : 0.0f;
    return out;
}

inline double thumbDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d += std::fabs(a[i] - b[i]);
    return d / a.size();
}

// Mean difference above which the picture counts as changed since Process.
constexpr double kThumbChanged = 0.03;

inline bool writeThumb(const std::string& path, const std::vector<float>& t) {
#ifdef _WIN32
    FILE* f = nullptr;
    _wfopen_s(&f, std::filesystem::u8path(path).c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = std::fwrite("CK2T", 1, 4, f) == 4 &&
              std::fwrite(t.data(), sizeof(float), t.size(), f) == t.size();
    return (std::fclose(f) == 0) && ok;
}

inline bool readThumb(const std::string& path, std::vector<float>& t) {
#ifdef _WIN32
    FILE* f = nullptr;
    _wfopen_s(&f, std::filesystem::u8path(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    char magic[4];
    t.assign(kThumbW * kThumbH * 3, 0.0f);
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "CK2T", 4) == 0 &&
              std::fread(t.data(), sizeof(float), t.size(), f) == t.size();
    std::fclose(f);
    return ok;
}

// The frames Process covers: the frames the source clip has, within the
// host's timeline bounds (when it gives any), then First..Last within that.
// Asking Resolve's Fusion page for a frame the clip doesn't have never
// returns, so the range must never go past the clip.
inline bool processRange(double clipFirst, double clipLast, double hostFirst, double hostLast,
                         bool useFirstLast, int firstFrame, int lastFrame, long& first, long& last) {
    // Fusion reports a clip's range as e.g. [1001, 1023.9999]: frames 1001 to
    // 1023. Rounding the end would ask for 1024, which the clip doesn't have.
    first = static_cast<long>(std::ceil(clipFirst - 1e-6));
    last = static_cast<long>(std::floor(clipLast + 1e-6));
    if (hostLast > hostFirst) {
        // Resolve can report the timeline's end one frame short when the clip's
        // frame rate differs from the timeline's (a 29.97 clip on a 23.976
        // timeline: comp 1001-1094, bounds 1001-1093). Allow one frame past it,
        // as long as the clip has that frame.
        first = std::max(first, std::lround(hostFirst));
        last = std::min(last, static_cast<long>(std::floor(hostLast + 1e-6)) + 1);
    }
    if (useFirstLast) {
        first = std::max(first, static_cast<long>(firstFrame));
        last = std::min(last, static_cast<long>(lastFrame));
    }
    return last >= first;
}

// Pulls a field out of one of the worker's JSON event lines. Enough for the
// flat objects the worker writes; returns "" when missing.
inline std::string jsonField(const std::string& line, const std::string& key) {
    std::string pat = "\"" + key + "\": ";
    size_t i = line.find(pat);
    if (i == std::string::npos) return "";
    i += pat.size();
    if (i < line.size() && line[i] == '"') {
        std::string out;
        for (size_t j = i + 1; j < line.size(); ++j) {
            if (line[j] == '\\' && j + 1 < line.size()) { out += line[++j]; continue; }
            if (line[j] == '"') break;
            out += line[j];
        }
        return out;
    }
    size_t j = line.find_first_of(",}", i);
    return line.substr(i, j == std::string::npos ? std::string::npos : j - i);
}

}  // namespace ck2
