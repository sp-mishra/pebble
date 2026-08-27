/**
 * @file akruti_spectral_km.hpp
 * @brief High-fidelity Kubelka–Munk spectral mixing utilities (38-band).
 *
 * Detailed reference for the `km38` spectral model:
 * - Purpose: reconstruct 38-band reflectance spectra from sRGB inputs, mix pigments using
 *   Kubelka–Munk concentration model, and convert between spectral, XYZ and perceptual (OKLab/OKLCh) spaces.
 * - Scope: header-only, templated numeric functions (Float concept), renderer-friendly PODs (RGBf / RGB8),
 *   color object with cached derived spaces, mixing, palette and gradient helpers.
 *
 * Key features:
 * - Deterministic conversions and mixing suitable for batch processing.
 * - Configurable knobs (gamut mapping, epsilon stability).
 * - Convenience wrappers (mix_rgb8, palette, gradient) for renderer pipelines.
 *
 * Usage:
 * @code
 * using namespace akruti::spectral::km38;
 * Config cfg;
 * Color<double> c(RGB8{255,0,0}, cfg);      // construct from 8-bit sRGB
 * auto shards = mix<double>({{&c,1.0}}, cfg); // mix single color (identity)
 * RGB8 out = c.toRGB8();
 * @endcode
 *
 * Notes on numerical robustness:
 * - Many operations use small epsilons (cfg.epsilon) and double-precision accumulation is used in public
 *   code paths (where templates are instantiated with double) to avoid catastrophic cancellation.
 */

#ifndef AKRUTI_SPECTRAL_KM_HPP
#define AKRUTI_SPECTRAL_KM_HPP

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <type_traits> // added earlier
// #include <expected>    // added for std::expected returned by fromPigmentName
#if defined(__has_include)
#  if __has_include(<expected>)
#    include <expected>    // optional in some standard libraries; guard for portability
#  endif
#endif
// Optional: include Google Highway only if available
#if __has_include(<hwy/highway.h>)
#include <hwy/highway.h>
#endif
 namespace akruti::spectral {
    /**
     * @brief Floating-point concept used across the header (float/double).
     *
     * All public templates constrain T with this concept to ensure numeric operations are well-defined.
     */
    template<typename T>
    concept Float = std::is_floating_point_v<T>;

    namespace km38 {
        /**
         * @brief km38: 38-band Kubelka–Munk model namespace
         *
         * Contains dataset (BASE spectra, CMF), conversion matrices and the high-level API
         * for conversions and mixing. All public API within this namespace is designed to be
         * header-only and templated on floating point (Float).
         */
        static constexpr std::size_t kSize = 38;
        static constexpr double kGamma = 2.4;

        // ------------------------------------------------------------
        // Small PODs
        // ------------------------------------------------------------
        /**
         * @brief Linear RGB float triple (range [0..1] by convention).
         */
        template<Float T = float>
        struct RGBf {
            T r{}, g{}, b{};
        };

        /**
         * @brief 8-bit integer RGB for renderer interop.
         */
        struct RGB8 {
            std::uint8_t r{}, g{}, b{};
        };

        // ------------------------------------------------------------
        // Configuration knobs
        // ------------------------------------------------------------
        /**
         * @brief Config structure controlling numeric stability and gamut mapping.
         *
         * - epsilon: tiny stability value used in KS/KM and reflectance guards.
         * - use_linear_srgb: kept for compatibility with companding helpers (unused by core numeric pipelines).
         * - gamut_jnd / gamut_e: parameters used by mapToGamut() binary search (JND threshold and epsilon).
         * - enable_gamut_map: enable/disable mapping vs clipping behavior in toRGB8/toHex.
         */
        struct Config {
            double epsilon = 1e-12;
            bool use_linear_srgb = true;
            double gamut_jnd = 0.03;
            double gamut_e = 0.0001;
            bool enable_gamut_map = true;
        };

        // ------------------------------------------------------------
        // Small internal helpers (detail)
        // ------------------------------------------------------------
        namespace detail {
            /**
             * @brief Clamp value to [lo,hi].
             */
            template<Float T>
            inline T clamp(T v, T lo, T hi) noexcept { return std::min(std::max(v, lo), hi); }

            /**
             * @brief Clamp to [0,1].
             */
            template<Float T>
            inline T clamp01(T v) noexcept { return clamp(v, T(0), T(1)); }

            /**
             * @brief Linear interpolation a + (b-a)*t.
             */
            template<Float T>
            inline T lerp(T a, T b, T t) noexcept { return a + (b - a) * t; }

            /**
             * @brief sRGB uncompanding (sRGB->linear) for a single channel.
             *
             * Implements the standard sRGB transfer curve.
             */
            template<Float T>
            inline T uncompand(T x) noexcept {
                return (x > T(0.04045)) ? std::pow((x + T(0.055)) / T(1.055), T(kGamma)) : x / T(12.92);
            }

            /**
             * @brief sRGB companding (linear->sRGB).
             */
            template<Float T>
            inline T compand(T x) noexcept {
                return (x > T(0.0031308)) ? T(1.055) * std::pow(x, T(1.0 / kGamma)) - T(0.055) : x * T(12.92);
            }

            /**
             * @brief Multiply 3x3 matrix by 3-vector.
             */
            template<Float T>
            inline std::array<T, 3> mul3x3(const std::array<std::array<T, 3>, 3> &m,
                                           const std::array<T, 3> &v) noexcept {
                return {
                    m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
                    m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                    m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
                };
            }

            /**
             * @brief Multiply 3x38 matrix by 38-vector (spectral -> XYZ/RGB).
             */
            template<Float T>
            inline std::array<T, 3> mul3xN(const std::array<std::array<T, kSize>, 3> &m,
                                           const std::array<T, kSize> &v) noexcept {
                std::array<T, 3> out{T(0), T(0), T(0)};
                for (std::size_t i = 0; i < kSize; ++i) {
                    out[0] += m[0][i] * v[i];
                    out[1] += m[1][i] * v[i];
                    out[2] += m[2][i] * v[i];
                }
                return out;
            }

            /**
             * @brief Check whether an lRGB triple is inside the canonical [0,1] cube (with epsilon).
             */
            template<Float T>
            inline bool in_gamut(const std::array<T, 3> &lrgb, T eps = T(0)) noexcept {
                return (lrgb[0] >= -eps && lrgb[0] <= T(1) + eps) &&
                       (lrgb[1] >= -eps && lrgb[1] <= T(1) + eps) &&
                       (lrgb[2] >= -eps && lrgb[2] <= T(1) + eps);
            }

            /**
             * @brief Euclidean delta E in OKLab space (simple metric).
             */
            template<Float T>
            inline T deltaE_ok(const std::array<T, 3> &oklab1, const std::array<T, 3> &oklab2) noexcept {
                const T dL = oklab1[0] - oklab2[0];
                const T da = oklab1[1] - oklab2[1];
                const T db = oklab1[2] - oklab2[2];
                return std::sqrt(dL * dL + da * da + db * db);
            }

            /**
             * @brief Convert OKLab -> OKLCh.
             * @return array {L, C, h(degrees)}
             */
            template<Float T>
            inline std::array<T, 3> oklab_to_oklch(const std::array<T, 3> &lab) noexcept {
                const T L = lab[0], a = lab[1], b = lab[2];
                const T C = std::sqrt(a * a + b * b);
                T h = std::atan2(b, a) * T(180.0 / 3.14159265358979323846);
                if (h < T(0)) h += T(360);
                return {L, C, h};
            }

            /**
             * @brief Convert OKLCh -> OKLab.
             * @param lch array {L, C, h(degrees)}
             * @return array {L, a, b}
             */
            template<Float T>
            inline std::array<T, 3> oklch_to_oklab(const std::array<T, 3> &lch) noexcept {
                const T L = lch[0], C = lch[1], h = lch[2];
                const T hr = h * T(3.14159265358979323846 / 180.0);
                const T a = C * std::cos(hr);
                const T b = C * std::sin(hr);
                return {L, a, b};
            }
        } // namespace detail

        // ------------------------------------------------------------
        // Dataset (BASE spectra, CMF, matrices)
        //  - Data contains compile-time arrays used for spectral reconstruction
        // ------------------------------------------------------------
        struct Data {
            // Base spectra
            static constexpr std::array<double, kSize> W = {
                1.00116072718764, 1.00116065159728, 1.00116031922747, 1.00115867270789, 1.00115259844552,
                1.00113252528998, 1.00108500663327, 1.00099687889453, 1.00086525152274,
                1.0006962900094, 1.00050496114888, 1.00030808187992, 1.00011966602013, 0.999952765968407,
                0.999821836899297, 0.999738609557593, 0.999709551639612, 0.999731930210627,
                0.999799436346195, 0.999900330316671, 1.00002040652611, 1.00014478793658, 1.00025997903412,
                1.00035579697089, 1.00042753780269, 1.00047623344888, 1.00050720967508,
                1.00052519156373, 1.00053509606896, 1.00054022097482, 1.00054272816784, 1.00054389569087,
                1.00054448212151, 1.00054476959992, 1.00054489887762, 1.00054496254689,
                1.00054498927058, 1.000544996993,
            };
            static constexpr std::array<double, kSize> C = {
                0.970585001322962, 0.970592498143425, 0.970625348729891, 0.970786806119017, 0.971368673228248,
                0.973163230621252, 0.976740223158765, 0.981587605491377, 0.986280265652949,
                0.989949147689134, 0.99249270153842, 0.994145680405256, 0.995183975033212, 0.995756750110818,
                0.99591281828671, 0.995606157834528, 0.994597600961854, 0.99221571549237,
                0.986236452783249, 0.967943337264541, 0.891285004244943, 0.536202477862053, 0.154108119001878,
                0.0574575093228929, 0.0315349873107007, 0.0222633920086335, 0.0182022841492439,
                0.016299055973264, 0.0153656239334613, 0.0149111568733976, 0.0146954339898235, 0.0145964146717719,
                0.0145470156699655, 0.0145228771899495, 0.0145120341118965,
                0.0145066940939832, 0.0145044507314479, 0.0145038009464639,
            };
            static constexpr std::array<double, kSize> M = {
                0.990673557319988, 0.990671524961979, 0.990662582353421, 0.990618107644795, 0.99045148087871,
                0.989871081400204, 0.98828660875964, 0.984290692797504, 0.973934905625306,
                0.941817838460145, 0.817390326195156, 0.432472805065729, 0.13845397825887, 0.0537347216940033,
                0.0292174996673231, 0.021313651750859, 0.0201349530181136, 0.0241323096280662,
                0.0372236145223627, 0.0760506552706601, 0.205375471942399, 0.541268903460439, 0.815841685086486,
                0.912817704123976, 0.946339830166962, 0.959927696331991, 0.966260595230312,
                0.969325970058424, 0.970854536721399, 0.971605066528128, 0.971962769757392, 0.972127272274509,
                0.972209417745812, 0.972249577678424, 0.972267621998742, 0.97227650946215,
                0.972280243306874, 0.97228132482656,
            };
            static constexpr std::array<double, kSize> Y = {
                0.0210523371789306, 0.0210564627517414, 0.0210746178695038, 0.0211649058448753, 0.0215027957272504,
                0.0226738799041561, 0.0258235649693629, 0.0334879385639851,
                0.0519069663740307, 0.100749014833473, 0.239129899706847, 0.534804312272748, 0.79780757864303,
                0.911449894067384, 0.953797963004507, 0.971241615465429, 0.979303123807588,
                0.983380119507575, 0.985461246567755, 0.986435046976605, 0.986738250670141, 0.986617882445032,
                0.986277776758643, 0.985860592444056, 0.98547492767621, 0.985176934765558,
                0.984971574014181, 0.984846303415712, 0.984775351811199, 0.984738066625265, 0.984719648311765,
                0.984711023391939, 0.984706683300676, 0.984704554393091, 0.98470359630937,
                0.984703124077552, 0.98470292561509, 0.984702868122795,
            };
            static constexpr std::array<double, kSize> R = {
                0.0315605737777207, 0.0315520718330149, 0.0315148215513658, 0.0313318044982702, 0.0306729857725527,
                0.0286480476989607, 0.0246450407045709, 0.0192960753663651,
                0.0142066612220556, 0.0102942608878609, 0.0076191460521811, 0.005898041083542, 0.0048233247781713,
                0.0042298748350633, 0.0040599171299341, 0.0043533695594676,
                0.0053434425970201, 0.0076917201010463, 0.0135969795736536, 0.0316975442661115, 0.107861196355249,
                0.463812603168704, 0.847055405272011, 0.943185409393918, 0.968862150696558,
                0.978030667473603, 0.982043643854306, 0.983923623718707, 0.984845484154382, 0.985294275814596,
                0.985507295219825, 0.985605071539837, 0.985653849933578, 0.985677685033883,
                0.985688391806122, 0.985693664690031, 0.985695879848205, 0.985696521463762,
            };
            static constexpr std::array<double, kSize> G = {
                0.0095560747554212, 0.0095581580120851, 0.0095673245444588, 0.0096129126297349, 0.0097837090401843,
                0.010378622705871, 0.0120026452378567, 0.0160977721473922,
                0.026706190223168, 0.0595555440185881, 0.186039826532826, 0.570579820116159, 0.861467768400292,
                0.945879089767658, 0.970465486474305, 0.97841363028445, 0.979589031411224,
                0.975533536908632, 0.962288755397813, 0.92312157451312, 0.793434018943111, 0.459270135902429,
                0.185574103666303, 0.0881774959955372, 0.05436302287667, 0.0406288447060719,
                0.034221520431697, 0.0311185790956966, 0.0295708898336134, 0.0288108739348928, 0.0284486271324597,
                0.0282820301724731, 0.0281988376490237, 0.0281581655342037,
                0.0281398910216386, 0.0281308901665811, 0.0281271086805816, 0.0281260133612096,
            };
            static constexpr std::array<double, kSize> B = {
                0.979404752502014, 0.97940070684313, 0.979382903470261, 0.979294364945594, 0.97896301460857,
                0.977814466694043, 0.974724321133836, 0.967198482343973, 0.949079657530575,
                0.900850128940977, 0.76315044546224, 0.465922171649319, 0.201263280451005, 0.0877524413419623,
                0.0457176793291679, 0.0284706050521843, 0.020527176756985, 0.0165302792310211,
                0.0145135107212858, 0.0136003508637687, 0.0133604258769571, 0.013548894314568, 0.0139594356366992,
                0.014443425575357, 0.0148854440621406, 0.0152254296999746,
                0.0154592848180209, 0.0156018026485961, 0.0156824871281936, 0.0157248764360615, 0.0157458108784121,
                0.0157556123350225, 0.0157605443964911, 0.0157629637515278,
                0.0157640525629106, 0.015764589232951, 0.0157648147772649, 0.0157648801149616,
            };

            // CIE Color Matching Functions weighted by D65 (3 x 38)
            static constexpr std::array<std::array<double, kSize>, 3> CMF = {
                {
                    {
                        {
                            0.0000646919989576, 0.0002194098998132, 0.0011205743509343, 0.0037666134117111,
                            0.011880553603799, 0.0232864424191771, 0.0345594181969747, 0.0372237901162006,
                            0.0324183761091486, 0.021233205609381, 0.0104909907685421, 0.0032958375797931,
                            0.0005070351633801, 0.0009486742057141, 0.0062737180998318, 0.0168646241897775,
                            0.028689649025981, 0.0426748124691731, 0.0562547481311377, 0.0694703972677158,
                            0.0830531516998291, 0.0861260963002257, 0.0904661376847769, 0.0850038650591277,
                            0.0709066691074488, 0.0506288916373645, 0.035473961885264, 0.0214682102597065,
                            0.0125164567619117, 0.0068045816390165, 0.0034645657946526, 0.0014976097506959,
                            0.000769700480928, 0.0004073680581315, 0.0001690104031614, 0.0000952245150365,
                            0.0000490309872958, 0.0000199961492222,
                        }
                    },
                    {
                        {
                            0.000001844289444, 0.0000062053235865, 0.0000310096046799, 0.0001047483849269,
                            0.0003536405299538, 0.0009514714056444, 0.0022822631748318, 0.004207329043473,
                            0.0066887983719014, 0.0098883960193565, 0.0152494514496311, 0.0214183109449723,
                            0.0334229301575068, 0.0513100134918512, 0.070402083939949, 0.0878387072603517,
                            0.0942490536184085, 0.0979566702718931, 0.0941521856862608, 0.0867810237486753,
                            0.0788565338632013, 0.0635267026203555, 0.05374141675682, 0.042646064357412,
                            0.0316173492792708, 0.020885205921391, 0.0138601101360152, 0.0081026402038399,
                            0.004630102258803, 0.0024913800051319, 0.0012593033677378, 0.000541646522168,
                            0.0002779528920067, 0.0001471080673854, 0.0000610327472927, 0.0000343873229523,
                            0.0000177059860053, 0.000007220974913,
                        }
                    },
                    {
                        {
                            0.000305017147638, 0.0010368066663574, 0.0053131363323992, 0.0179543925899536,
                            0.0570775815345485, 0.113651618936287, 0.17335872618355, 0.196206575558657,
                            0.186082370706296, 0.139950475383207, 0.0891745294268649, 0.0478962113517075,
                            0.0281456253957952, 0.0161376622950514, 0.0077591019215214, 0.0042961483736618,
                            0.0020055092122156, 0.0008614711098802, 0.0003690387177652, 0.0001914287288574,
                            0.0001495555858975, 0.0000923109285104, 0.0000681349182337, 0.0000288263655696,
                            0.0000157671820553, 0.0000039406041027, 0.000001584012587, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                            0.0, 0.0, 0.0, 0.0, 0.0,
                        }
                    }
                }
            };

            // Conversion matrices (same as JS)
            static constexpr std::array<std::array<double, 3>, 3> RGB_XYZ = {
                {
                    {0.41239079926595934, 0.357584339383878, 0.1804807884018343},
                    {0.21263900587151027, 0.715168678767756, 0.07219231536073371},
                    {0.01933081871559182, 0.11919477979462598, 0.9505321522496607}
                }
            };
            static constexpr std::array<std::array<double, 3>, 3> XYZ_RGB = {
                {
                    {3.2409699419045226, -1.537383177570094, -0.4986107602930034},
                    {-0.9692436362808796, 1.8759675015077202, 0.04155505740717559},
                    {0.05563007969699366, -0.20397695888897652, 1.0569715142428786}
                }
            };

            static constexpr std::array<std::array<double, 3>, 3> XYZ_LMS = {
                {
                    {0.819022437996703, 0.3619062600528904, -0.1288737815209879},
                    {0.0329836539323885, 0.9292868615863434, 0.0361446663506424},
                    {0.0481771893596242, 0.2642395317527308, 0.6335478284694309}
                }
            };
            static constexpr std::array<std::array<double, 3>, 3> LMS_XYZ = {
                {
                    {1.2268798758459243, -0.5578149944602171, 0.2813910456659647},
                    {-0.0405757452148008, 1.112286803280317, -0.0717110580655164},
                    {-0.0763729366746601, -0.4214933324022432, 1.5869240198367816}
                }
            };
            static constexpr std::array<std::array<double, 3>, 3> LMS_LAB = {
                {
                    {0.210454268309314, 0.7936177747023054, -0.0040720430116193},
                    {1.9779985324311684, -2.4285922420485799, 0.450593709617411},
                    {0.0259040424655478, 0.7827717124575296, -0.8086757549230774}
                }
            };
            static constexpr std::array<std::array<double, 3>, 3> LAB_LMS = {
                {
                    {1.0, 0.3963377773761749, 0.2158037573099136},
                    {1.0, -0.1055613458156586, -0.0638541728258133},
                    {1.0, -0.0894841775298119, -1.2914855480194092}
                }
            };
        };

        // ------------------------------------------------------------
        // Core math (KS/KM, conversions)
        // ------------------------------------------------------------

        /**
         * @brief Convert reflectance R (scalar in [0,1]) to KS (Kubelka structural parameter).
         *
         * KS = (1-R)^2 / (2R), with clamping to avoid division by zero.
         *
         * @tparam T floating point type
         * @param R reflectance [0..1]
         * @param cfg configuration (epsilon guard)
         * @return KS value (>=0)
         */
        template<Float T>
        inline T KS(T R, const Config &cfg) noexcept {
            // JS: (1-R)^2/(2R)
            R = detail::clamp(R, T(cfg.epsilon), T(1) - T(cfg.epsilon));
            const T one_minus = T(1) - R;
            return (one_minus * one_minus) / (T(2) * R);
        }

        /**
         * @brief Inverse operation KM(ks) approximating reflectance from KS.
         *
         * KM = 1 + ks - sqrt(ks^2 + 2*ks)
         *
         * @tparam T floating point type
         * @param ks Kubelka structure parameter (>=0)
         * @return reflectance-like value
         */
        template<Float T>
        inline T KM(T ks) noexcept {
            // Numerically-stable inversion of KS -> R:
            // Use rationalized form R = 1 / (1 + ks + sqrt(ks^2 + 2ks))
            // and ensure a sensible lower bound to match behaviour used elsewhere
            // (many hot paths clamp to cfg.epsilon). We choose the library default
            // epsilon (1e-12) as a minimal clamp for the standalone KM() helper so
            // callers that don't have a Config still get robust values.
            ks = std::max(ks, T(0));
            const T s = std::sqrt(ks * ks + T(2) * ks);
            const T denom = T(1) + ks + s;
            const T r = T(1) / denom;
            constexpr T default_eps = static_cast<T>(1e-12);
            return std::min(T(1), std::max(default_eps, r));
        }

        /**
         * @brief Convert 8-bit sRGB (0..255) to linear lRGB (0..1).
         * @tparam T floating point
         * @param sRGB_255 array of 3 channel values in [0..255]
         * @return linear lRGB in [0..1]
         */
        template<Float T>
        inline std::array<T, 3> sRGB_to_lRGB(const std::array<T, 3> &sRGB_255) noexcept {
            return {
                detail::uncompand(sRGB_255[0] / T(255)),
                detail::uncompand(sRGB_255[1] / T(255)),
                detail::uncompand(sRGB_255[2] / T(255)),
            };
        }

        /**
         * @brief Convert linear lRGB to sRGB 0..255 rounded values.
         */
        template<Float T>
        inline std::array<T, 3> lRGB_to_sRGB(const std::array<T, 3> &lRGB) noexcept {
            return {
                std::round(detail::compand(lRGB[0]) * T(255)),
                std::round(detail::compand(lRGB[1]) * T(255)),
                std::round(detail::compand(lRGB[2]) * T(255)),
            };
        }

        // Conversions lRGB <-> XYZ <-> OKLab
        template<Float T>
        inline std::array<T, 3> lRGB_to_XYZ(const std::array<T, 3> &lRGB) noexcept {
            const auto m = Data::RGB_XYZ;
            std::array<std::array<T, 3>, 3> mt{
                {
                    {T(m[0][0]), T(m[0][1]), T(m[0][2])},
                    {T(m[1][0]), T(m[1][1]), T(m[1][2])},
                    {T(m[2][0]), T(m[2][1]), T(m[2][2])},
                }
            };
            return detail::mul3x3(mt, lRGB);
        }

        template<Float T>
        inline std::array<T, 3> XYZ_to_lRGB(const std::array<T, 3> &XYZ) noexcept {
            const auto m = Data::XYZ_RGB;
            std::array<std::array<T, 3>, 3> mt{
                {
                    {T(m[0][0]), T(m[0][1]), T(m[0][2])},
                    {T(m[1][0]), T(m[1][1]), T(m[1][2])},
                    {T(m[2][0]), T(m[2][1]), T(m[2][2])},
                }
            };
            return detail::mul3x3(mt, XYZ);
        }

        template<Float T>
        inline std::array<T, 3> XYZ_to_OKLab(const std::array<T, 3> &XYZ) noexcept {
            // JS:
            // lms = mul(XYZ_LMS, XYZ).map(cbrt)
            // OKLab = mul(LMS_LAB, lms)
            const auto a = Data::XYZ_LMS;
            std::array<std::array<T, 3>, 3> A{
                {
                    {T(a[0][0]), T(a[0][1]), T(a[0][2])},
                    {T(a[1][0]), T(a[1][1]), T(a[1][2])},
                    {T(a[2][0]), T(a[2][1]), T(a[2][2])},
                }
            };
            auto lms = detail::mul3x3(A, XYZ);
            lms[0] = std::cbrt(lms[0]);
            lms[1] = std::cbrt(lms[1]);
            lms[2] = std::cbrt(lms[2]);

            const auto b = Data::LMS_LAB;
            std::array<std::array<T, 3>, 3> B{
                {
                    {T(b[0][0]), T(b[0][1]), T(b[0][2])},
                    {T(b[1][0]), T(b[1][1]), T(b[1][2])},
                    {T(b[2][0]), T(b[2][1]), T(b[2][2])},
                }
            };
            return detail::mul3x3(B, lms);
        }

        template<Float T>
        inline std::array<T, 3> OKLab_to_XYZ(const std::array<T, 3> &OKLab) noexcept {
            // JS:
            // lms = mul(LAB_LMS, OKLab).map(x^3)
            // XYZ = mul(LMS_XYZ, lms)
            const auto a = Data::LAB_LMS;
            std::array<std::array<T, 3>, 3> A{
                {
                    {T(a[0][0]), T(a[0][1]), T(a[0][2])},
                    {T(a[1][0]), T(a[1][1]), T(a[1][2])},
                    {T(a[2][0]), T(a[2][1]), T(a[2][2])},
                }
            };
            auto lms = detail::mul3x3(A, OKLab);
            lms[0] = lms[0] * lms[0] * lms[0];
            lms[1] = lms[1] * lms[1] * lms[1];
            lms[2] = lms[2] * lms[2] * lms[2];

            const auto b = Data::LMS_XYZ;
            std::array<std::array<T, 3>, 3> B{
                {
                    {T(b[0][0]), T(b[0][1]), T(b[0][2])},
                    {T(b[1][0]), T(b[1][1]), T(b[1][2])},
                    {T(b[2][0]), T(b[2][1]), T(b[2][2])},
                }
            };
            return detail::mul3x3(B, lms);
        }

        template<Float T>
        inline std::array<T, 3> lRGB_to_OKLab(const std::array<T, 3> &lRGB) noexcept {
            return XYZ_to_OKLab(lRGB_to_XYZ(lRGB));
        }

        template<Float T>
        inline std::array<T, 3> OKLab_to_lRGB(const std::array<T, 3> &OKLab) noexcept {
            return XYZ_to_lRGB(OKLab_to_XYZ(OKLab));
        }

        /**
         * @brief Construct XYZ from 38-band reflectance samples via CMF weighting.
         *
         * @param R 38-band reflectance vector
         * @return XYZ (linear)
         */
        template<Float T>
        inline std::array<T, 3> R_to_XYZ(const std::array<T, kSize> &R) noexcept {
            const auto cmf = Data::CMF;
            std::array<std::array<T, kSize>, 3> M{
                {
                    [&] {
                        std::array<T, kSize> r{};
                        for (std::size_t i = 0; i < kSize; ++i) r[i] = T(cmf[0][i]);
                        return r;
                    }(),
                    [&] {
                        std::array<T, kSize> r{};
                        for (std::size_t i = 0; i < kSize; ++i) r[i] = T(cmf[1][i]);
                        return r;
                    }(),
                    [&] {
                        std::array<T, kSize> r{};
                        for (std::size_t i = 0; i < kSize; ++i) r[i] = T(cmf[2][i]);
                        return r;
                    }(),
                }
            };
            return detail::mul3xN(M, R);
        }

        /**
         * @brief Reconstruct 38-band reflectance R from linear lRGB input using BASE spectra.
         *
         * Algorithm summary (from JS):
         *  - Remove white component (w = min(lRGB)).
         *  - Derive c/m/y and rr/gg/bb contributions.
         *  - Compose bands as weighted sum of base spectra.
         *
         * @param lRGB linear RGB triple
         * @param cfg config for epsilon guards
         * @return 38-band reflectance array
         */
        template<Float T>
        inline std::array<T, kSize> lRGB_to_R(std::array<T, 3> lRGB, const Config &cfg) noexcept {
            // JS:
            // w = min(lRGB)
            // lRGB -= w
            // c = min(g,b); m = min(r,b); y = min(r,g)
            // r = max(0, min(r-b, r-g))
            // g = max(0, min(g-b, g-r))
            // b = max(0, min(b-g, b-r))
            T w = std::min({lRGB[0], lRGB[1], lRGB[2]});
            T r_rem = lRGB[0] - w;
            T g_rem = lRGB[1] - w;
            T b_rem = lRGB[2] - w;

            // Subtractive secondary filters (absorb one primary each)
            const T c = std::min(g_rem, b_rem);
            const T m = std::min(r_rem, b_rem);
            const T y = std::min(r_rem, g_rem);

            // Primary residuals
            const T rr = std::max(T(0), r_rem - m - y);
            const T gg = std::max(T(0), g_rem - c - y);
            const T bb = std::max(T(0), b_rem - c - m);

            std::array<T, kSize> R{};
            for (std::size_t i = 0; i < kSize; ++i) {
                const T v =
                        w * T(Data::W[i]) +
                        c * T(Data::C[i]) +
                        m * T(Data::M[i]) +
                        y * T(Data::Y[i]) +
                        rr * T(Data::R[i]) +
                        gg * T(Data::G[i]) +
                        bb * T(Data::B[i]);

                R[i] = std::max(T(cfg.epsilon), v);
            }
            return R;
        }

        // ------------------------------------------------------------
        // Color object: caches derived spaces and provides high-level utilities
        // ------------------------------------------------------------
        /**
         * @brief Color<T> wraps a color representation with cached spectral and perceptual spaces.
         *
         * Construction forms:
         *  - Color(RGB8)         : from 8-bit sRGB
         *  - Color(array<3>)     : from sRGB floats (0..255)
         *  - Color(array<38>)    : from reflectance samples (R bands)
         *
         * Main capabilities:
         *  - Access sRGB(), lRGB(), R(), XYZ()
         *  - OKLab/OKLCh conversion helpers
         *  - inGamut(), clipToGamut(), mapToGamut() (binary-search chroma)
         *  - toRGB8/toHex convenience outputs
         */
        template<Float T = double>
        class Color {
        public:
            using scalar = T;

            /** Construct from 8-bit sRGB. */
            explicit Color(RGB8 rgb8, const Config &cfg = {}) : cfg_(cfg) {
                sRGB_ = {T(rgb8.r), T(rgb8.g), T(rgb8.b)};
                lRGB_ = sRGB_to_lRGB(sRGB_);
                R_ = lRGB_to_R(lRGB_, cfg_);
                XYZ_ = R_to_XYZ(R_);
            }

            /** Construct from sRGB floats in [0..255]. */
            explicit Color(const std::array<T, 3> &sRGB_255, const Config &cfg = {}) : cfg_(cfg) {
                sRGB_ = sRGB_255;
                lRGB_ = sRGB_to_lRGB(sRGB_);
                R_ = lRGB_to_R(lRGB_, cfg_);
                XYZ_ = R_to_XYZ(R_);
            }

            /** Construct directly from reflectance bands (38-band). */
            explicit Color(const std::array<T, kSize> &R, const Config &cfg = {}) : cfg_(cfg), R_(R) {
                XYZ_ = R_to_XYZ(R_);
                lRGB_ = XYZ_to_lRGB(XYZ_);
                sRGB_ = lRGB_to_sRGB(lRGB_);
            }

            /** Return sRGB (0..255 floats). */
            const std::array<T, 3> &sRGB() const noexcept { return sRGB_; }

            /** Return linear lRGB ([0..1]) */
            const std::array<T, 3> &lRGB() const noexcept { return lRGB_; }

            /** Return underlying reflectance bands. */
            const std::array<T, kSize> &R() const noexcept { return R_; }

            /** Compute OKLab (lazy cached). */
            std::array<T, 3> OKLab() const noexcept {
                if (!oklab_valid_) {
                    oklab_ = XYZ_to_OKLab(XYZ_);
                    oklab_valid_ = true;
                }
                return oklab_;
            }

            /** Compute OKLCh (lazy cached). */
            std::array<T, 3> OKLCh() const noexcept {
                if (!oklch_valid_) {
                    oklch_ = detail::oklab_to_oklch(OKLab());
                    oklch_valid_ = true;
                }
                return oklch_;
            }

            /** Compute KS per band (lazy) */
            std::array<T, kSize> KS_values() const noexcept {
                std::array<T, kSize> out{};
                for (std::size_t i = 0; i < kSize; ++i) out[i] = KS(R_[i], cfg_);
                return out;
            }

            /** Luminance helper (XYZ[1] guard). */
            T luminance() const noexcept {
                return std::max(T(cfg_.epsilon), XYZ_[1]);
            }

            /** tintingStrength property (JS default=1) */
            T tintingStrength() const noexcept { return tinting_strength_; }
            void setTintingStrength(T ts) noexcept { tinting_strength_ = ts; }

            /** inGamut check (linear RGB). */
            bool inGamut(T eps = T(0)) const noexcept {
                return detail::in_gamut(lRGB_, eps);
            }

            /** Clip sRGB to gamut by clamping channels. */
            Color clipToGamut() const {
                std::array<T, 3> s = sRGB_;
                s[0] = detail::clamp(s[0], T(0), T(255));
                s[1] = detail::clamp(s[1], T(0), T(255));
                s[2] = detail::clamp(s[2], T(0), T(255));
                return Color(s, cfg_);
            }

            /**
             * @brief Map to gamut in OKLCh chroma dimension using binary search.
             *
             * This method mirrors the JS `gamutMap()`:
             * - If already in gamut, returns *this.
             * - Otherwise performs bisection on chroma to find the chroma that maps within
             *   deltaE tolerance (cfg.gamut_jnd) of the unclipped OKLab value.
             *
             * Implementation notes:
             * - All intermediate vectors use template type T.
             * - Binary search terminates when maxC-minC < cfg.gamut_e.
             */
            Color mapToGamut() const {
                // Mirrors JS gamutMap()
                const auto lch = OKLCh();
                const T L = lch[0];

                if (L >= T(1)) return Color(std::array<T, 3>{T(255), T(255), T(255)}, cfg_);
                if (L <= T(0)) return Color(std::array<T, 3>{T(0), T(0), T(0)}, cfg_);

                if (inGamut()) return *this;

                const T h = lch[2];
                T minC = T(0);
                T maxC = lch[1];
                bool min_in_gamut = true;

                std::array<T, 3> current = lRGB_;
                // Construct an explicit std::array<T,3> so the template parameter T can be deduced.
                std::array<T, 3> clipped_rgb = {
                    detail::clamp01(current[0]),
                    detail::clamp01(current[1]),
                    detail::clamp01(current[2]),
                };
                std::array<T, 3> clipped_lab = lRGB_to_OKLab<T>(clipped_rgb);

                T E = detail::deltaE_ok(clipped_lab, lRGB_to_OKLab(current));
                if (E < T(cfg_.gamut_jnd)) {
                    // JS returns new Color(lRGB_to_sRGB(XYZ_to_lRGB(OKLab_to_XYZ(clipped))))
                    const auto lrgb2 = OKLab_to_lRGB(clipped_lab);
                    const auto srgb2 = lRGB_to_sRGB(lrgb2);
                    return Color(srgb2, cfg_);
                }

                const T e = T(cfg_.gamut_e);
                const T jnd = T(cfg_.gamut_jnd);

                while (maxC - minC > e) {
                    const T chroma = (minC + maxC) / T(2);

                    // explicit template argument + std::array to allow template deduction
                    const auto lab = detail::oklch_to_oklab<T>(std::array<T,3>{ L, chroma, h });

                    const auto xyz = OKLab_to_XYZ(lab);
                    current = XYZ_to_lRGB(xyz);

                    if (min_in_gamut && detail::in_gamut(current)) {
                        minC = chroma;
                    } else {
                        // Explicitly construct an array of type T so the template parameter T can be deduced.
                        clipped_lab = lRGB_to_OKLab<T>( std::array<T,3>{
                            detail::clamp01(current[0]),
                            detail::clamp01(current[1]),
                            detail::clamp01(current[2])
                        } );
                        E = detail::deltaE_ok(clipped_lab, lab);

                        if (E < jnd) {
                            if (jnd - E < e) break;
                            min_in_gamut = false;
                            minC = chroma;
                        } else {
                            maxC = chroma;
                        }
                    }
                }

                const auto lrgb2 = OKLab_to_lRGB(clipped_lab);
                const auto srgb2 = lRGB_to_sRGB(lrgb2);
                return Color(srgb2, cfg_);
            }

            /** Convert to 8-bit RGB with optional gamut mapping. */
            RGB8 toRGB8(bool gamut_map = true) const {
                Color c = *this;
                if (!c.inGamut()) {
                    c = gamut_map && cfg_.enable_gamut_map ? c.mapToGamut() : c.clipToGamut();
                }
                auto s = c.sRGB();
                auto q = [](T x) -> std::uint8_t {
                    x = detail::clamp(x, T(0), T(255));
                    return static_cast<std::uint8_t>(std::lround(x));
                };
                return {q(s[0]), q(s[1]), q(s[2])};
            }

            /** Hex string representation "#RRGGBB". */
            std::string toHex(bool gamut_map = true) const {
                const RGB8 c = toRGB8(gamut_map);
                static constexpr char hexdig[] = "0123456789ABCDEF";
                std::string out;
                out.reserve(7);
                out.push_back('#');
                out.push_back(hexdig[(c.r >> 4) & 0xF]);
                out.push_back(hexdig[c.r & 0xF]);
                out.push_back(hexdig[(c.g >> 4) & 0xF]);
                out.push_back(hexdig[c.g & 0xF]);
                out.push_back(hexdig[(c.b >> 4) & 0xF]);
                out.push_back(hexdig[c.b & 0xF]);
                return out;
            }

        private:
            Config cfg_{};
            std::array<T, 3> sRGB_{T(0), T(0), T(0)};
            std::array<T, 3> lRGB_{T(0), T(0), T(0)};
            std::array<T, kSize> R_{};
            std::array<T, 3> XYZ_{T(0), T(0), T(0)};

            mutable bool oklab_valid_ = false;
            mutable bool oklch_valid_ = false;
            mutable std::array<T, 3> oklab_{};
            mutable std::array<T, 3> oklch_{};

            T tinting_strength_ = T(1);
        };

        // ------------------------------------------------------------
        // SIMD helpers (optional Google Highway) with scalar fallback
        // ------------------------------------------------------------
        namespace detail {
#if __has_include(<hwy/highway.h>)
            // SIMD acceleration using Google Highway; guarded include keeps header portable.
            // The Highway header is already included at top-level; no need to include again here.

            // Hot-path helper: compute KS for 38 bands with epsilon clamp.
            // SIMD vs scalar: results match scalar within small floating tolerances.
            template<Float T>
            inline void ks_simd(const std::array<T, kSize>& R_in,
                                std::array<T, kSize>& ks_out,
                                const Config& cfg) noexcept {
                using D = hwy::HWY_NAMESPACE::ScalableTag<T>;
                const D d;
                const std::size_t L = hwy::HWY_NAMESPACE::Lanes(d);
                const auto epsv = hwy::HWY_NAMESPACE::Set(d, static_cast<T>(cfg.epsilon));
                const auto onev = hwy::HWY_NAMESPACE::Set(d, T(1));
                const auto twov = hwy::HWY_NAMESPACE::Set(d, T(2));

                std::size_t i = 0;
                for (; i + L <= kSize; i += L) {
                    const T* ptr = R_in.data() + i;
                    auto r = hwy::HWY_NAMESPACE::LoadU(d, ptr);
                    r = hwy::HWY_NAMESPACE::Max(r, epsv);                     // clamp R >= epsilon
                    r = hwy::HWY_NAMESPACE::Min(r, hwy::HWY_NAMESPACE::Sub(onev, epsv)); // clamp R <= 1 - epsilon
                    const auto one_minus = hwy::HWY_NAMESPACE::Sub(onev, r);
                    const auto num = hwy::HWY_NAMESPACE::Mul(one_minus, one_minus);
                    const auto denom = hwy::HWY_NAMESPACE::Mul(twov, r);
                    const auto ks = hwy::HWY_NAMESPACE::Div(num, denom);
                    hwy::HWY_NAMESPACE::StoreU(ks, d, ks_out.data() + i);
                }
                // Tail (scalar)
                for (; i < kSize; ++i) {
                    T R = std::min(std::max(R_in[i], static_cast<T>(cfg.epsilon)), T(1) - static_cast<T>(cfg.epsilon));
                    const T one_minus = T(1) - R;
                    ks_out[i] = (one_minus * one_minus) / (T(2) * R);
                }
            }

            // Hot-path helper: invert KS -> R with clamp to [epsilon, 1].
            template<Float T>
            inline void km_simd(const std::array<T, kSize>& ks_in,
                                std::array<T, kSize>& R_out,
                                const Config& cfg) noexcept {
                using D = hwy::HWY_NAMESPACE::ScalableTag<T>;
                const D d;
                const std::size_t L = hwy::HWY_NAMESPACE::Lanes(d);
                const auto zerov = hwy::HWY_NAMESPACE::Set(d, T(0));
                const auto onev = hwy::HWY_NAMESPACE::Set(d, T(1));
                const auto twov = hwy::HWY_NAMESPACE::Set(d, T(2));
                const auto epsv = hwy::HWY_NAMESPACE::Set(d, static_cast<T>(cfg.epsilon));

                std::size_t i = 0;
                for (; i + L <= kSize; i += L) {
                    const T* ptr = ks_in.data() + i;
                    auto ks = hwy::HWY_NAMESPACE::LoadU(d, ptr);
                    ks = hwy::HWY_NAMESPACE::Max(ks, zerov);
                    const auto ksq = hwy::HWY_NAMESPACE::Mul(ks, ks);
                    const auto inside = hwy::HWY_NAMESPACE::Add(ksq, hwy::HWY_NAMESPACE::Mul(twov, ks));
                    const auto s = hwy::HWY_NAMESPACE::Sqrt(inside);
                    // rationalized form: R = 1 / (1 + ks + s)
                    const auto denom = hwy::HWY_NAMESPACE::Add(hwy::HWY_NAMESPACE::Add(onev, ks), s);
                    auto R = hwy::HWY_NAMESPACE::Div(onev, denom);
                    R = hwy::HWY_NAMESPACE::Max(R, epsv);
                    R = hwy::HWY_NAMESPACE::Min(R, onev);
                    hwy::HWY_NAMESPACE::StoreU(R, d, R_out.data() + i);
                }
                // Tail (scalar)
                for (; i < kSize; ++i) {
                    const T ks = std::max(ks_in[i], T(0));
                    const T s = std::sqrt(ks * ks + T(2) * ks);
                    const T v = T(1) / (T(1) + ks + s);
                    R_out[i] = std::min(T(1), std::max(static_cast<T>(cfg.epsilon), v));
                }
            }
#else
            // Scalar fallbacks when Highway is not available.
            template<Float T>
            inline void ks_simd(const std::array<T, kSize>& R_in,
                                std::array<T, kSize>& ks_out,
                                const Config& cfg) noexcept {
                for (std::size_t i = 0; i < kSize; ++i) {
                    T R = std::min(std::max(R_in[i], static_cast<T>(cfg.epsilon)), T(1) - static_cast<T>(cfg.epsilon));
                    const T one_minus = T(1) - R;
                    ks_out[i] = (one_minus * one_minus) / (T(2) * R);
                }
            }

            template<Float T>
            inline void km_simd(const std::array<T, kSize>& ks_in,
                                std::array<T, kSize>& R_out,
                                const Config& cfg) noexcept {
                for (std::size_t i = 0; i < kSize; ++i) {
                    const T ks = std::max(ks_in[i], T(0));
                    const T s = std::sqrt(ks * ks + T(2) * ks);
                    const T v = T(1) / (T(1) + ks + s);
                    R_out[i] = std::min(T(1), std::max(static_cast<T>(cfg.epsilon), v));
                }
            }
#endif
        } // namespace detail

        // ------------------------------------------------------------
        // Mixing primitives & utilities
        // ------------------------------------------------------------
        template<Float T = double>
        struct MixItem {
            const Color<T> *color = nullptr;
            T factor = T(1);
        };

        /**
         * @brief mix(items) combines colors using JS-like Kubelka–Munk concentration model.
         *
         * SIMD vs scalar:
         * - The hot per-band KS/KM loops use detail::ks_simd and detail::km_simd.
         * - Behaviour matches the prior scalar path within small floating tolerances.
         *
         * Two-pass to avoid heap allocations; mixes are typically small; improves realtime painting performance.
         */
        template<Float T = double>
        inline Color<T> mix(std::span<const MixItem<T>> items, const Config &cfg = {}) {
            std::array<T, kSize> R{};
            std::array<T, kSize> ksMix{};
            for (std::size_t i = 0; i < kSize; ++i) ksMix[i] = T(0);

            // Inline lambda to compute effective concentration
            const auto effective_c = [](const MixItem<T>& it) -> T {
                if (!it.color) return T(0);
                return std::max(T(0), it.factor);
            };

            // Pass 1: compute total concentration.
            T totalC = T(0);
            for (const auto &it : items) {
                const T conc = effective_c(it);
                if (conc > T(0)) totalC += conc;
            }

            // If totalC == 0, match previous behavior: avg KS = 0 -> R = KM(0) clamped with epsilon.
            if (!(totalC > T(0))) {
                for (std::size_t i = 0; i < kSize; ++i) {
                    R[i] = std::max(T(cfg.epsilon), KM<T>(T(0)));
                }
                return Color<T>(R, cfg);
            }

            // Pass 2: accumulate KS per band using same math (SIMD helpers per item).
            // We convert each item's reflectance R->KS once into a small stack array, scale by conc, accumulate.
            for (const auto &it : items) {
                if (!it.color) continue;
                const T conc = effective_c(it);
                if (!(conc > T(0))) continue;

                // Per-item KS spectrum on stack; SIMD helper fills it.
                std::array<T, kSize> ks_tmp{};
                detail::ks_simd<T>(it.color->R(), ks_tmp, cfg);

                // Accumulate scaled KS into ksMix.
                for (std::size_t i = 0; i < kSize; ++i) {
                    ksMix[i] += ks_tmp[i] * conc;
                }
            }

            // Normalize KS by total concentration.
            std::array<T, kSize> ks_avg{};
            const T invTotal = T(1) / totalC;
            for (std::size_t i = 0; i < kSize; ++i) {
                ks_avg[i] = ksMix[i] * invTotal;
            }

            // Invert KS -> R with SIMD helper; helper clamps to [epsilon,1].
            detail::km_simd<T>(ks_avg, R, cfg);

            return Color<T>(R, cfg);
        }

        template<Float T = double>
        inline Color<T> mix(std::initializer_list<MixItem<T> > items, const Config &cfg = {}) {
            return mix<T>(std::span<const MixItem<T>>(items.begin(), items.size()), cfg);
        }

        /**
         * @brief Glaze (layered) a top pigment over a base pigment.
         *
         * Simplified per-band layered model:
         * - Convert per-band reflectances R -> KS using KS().
         * - Compute a thickness-controlled weighting w(thickness) using a normalized exponential easing
         *   so that small thickness preserves the base and larger thicknesses let the top coat contribute.
         * - Add the top's optical contribution to the base: ks_total = ks_base + ks_top * w.
         * - Convert back to reflectance via KM(ks_total) and clamp with cfg.epsilon.
         *
         * Notes & limitations:
         * - This is a simple, efficient approximation of a layered effect. It is NOT a full
         *   radiative-transfer / two-stream layered KM solution. It treats the top coat as an added
         *   optical thickness rather than solving the exact two-layer KM composition.
         * - The function operates per-band (38) and avoids heap allocation inside the hot loop.
         *
         * API:
         *   Color<T> glaze(const Color<T>& base, const Color<T>& top, T thickness, const Config& cfg)
         * thickness in [0,1] where 0 = no effect (returns base), 1 = strong top coat (top dominates).
         */
        template<Float T = double>
        inline Color<T> glaze(const Color<T> &base, const Color<T> &top, T thickness, const Config &cfg = {}) {
            // clamp thickness
            thickness = detail::clamp01(thickness);

            // normalized exponential ease: s(0)=0, s(1)=1, curve controlled by alpha (>0).
            // alpha controls how quickly the top contribution ramps up; ~6 gives a perceptually fast ramp.
            constexpr T alpha = T(6);
            const T denom = std::exp(alpha) - T(1);
            const T w = (denom > T(0)) ? (std::exp(alpha * thickness) - T(1)) / denom : thickness;

            std::array<T, kSize> R{};
            const auto &Rb = base.R();
            const auto &Rt = top.R();
            for (std::size_t i = 0; i < kSize; ++i) {
                // convert to KS with epsilon guards
                const T ks_b = KS(Rb[i], cfg);
                const T ks_t = KS(Rt[i], cfg);

                // layered approximation: add top optical thickness scaled by w
                const T ks_total = ks_b + ks_t * w;

                // back to reflectance and clamp
                R[i] = std::max(T(cfg.epsilon), KM(ks_total));
            }
            return Color<T>(R, cfg);
        }

        /*
         * Tests-as-comments:
         *
         * - thickness = 0 -> returns base approximately:
         *     auto c = glaze(base, top, 0.0, cfg);
         *     for each band i: approx_equal(c.R()[i], base.R()[i]) within a small tolerance.
         *
         * - thickness = 1 -> top has strong dominance (approximate, depends on base/top KS magnitudes):
         *     auto c = glaze(base, top, 1.0, cfg);
         *     Expect c to be significantly shifted towards top; in many practical pigment pairs
         *     the top contribution will dominate because ks_total = ks_b + ks_t.
         */

        /**
         * @brief Write a spectral KM palette into a caller-provided buffer (no heap).
         *
         * Usage (stack buffer, zero-heap):
         *   std::array<akruti::spectral::km38::Color<double>, 16> buf;
         *   palette_into<double>(std::span(buf), c1, c2, cfg); // no heap
         *
         * Follows the same concentration weighting as the vector palette():
         *   For i in [0..n-1], fa = (n - 1 - i), fb = i
         * Edge cases:
         *   n == 0 -> returns empty span; n == 1 -> {a}
         */
        template<Float T = double>
        inline std::span<Color<T>> palette_into(std::span<Color<T>> out,
                                                const Color<T>& a,
                                                const Color<T>& b,
                                                const Config& cfg = {}) noexcept {
            const std::size_t n = out.size();
            if (n == 0) return out.first(0);
            if (n == 1) { out[0] = a; return out.first(1); }

            for (std::size_t i = 0; i < n; ++i) {
                const T fa = T(n - 1 - i);
                const T fb = T(i);
                const std::array<MixItem<T>, 2> items = {
                    MixItem<T>{ &a, fa },
                    MixItem<T>{ &b, fb }
                };
                out[i] = mix<T>(std::span<const MixItem<T>>(items.data(), items.size()), cfg);
            }
            return out;
        }

        /**
         * @brief Generate a palette of `size` colors by interpolating concentrations.
         * @return vector of Color<T> of length `size`.
         *
         * Note: This allocates by design. For zero-heap, use palette_into().
         */
        template<Float T = double>
        inline std::vector<Color<T> > palette(const Color<T> &a, const Color<T> &b, std::size_t size,
                                              const Config &cfg = {}) {
            // Avoid default-constructing Color<T> (no default ctor). Resize with a fill value, then overwrite.
            std::vector<Color<T> > p;
            p.resize(size, a);
            // Delegate actual generation to the no-heap variant using identical concentration weights.
            palette_into<T>(std::span<Color<T>>(p.data(), p.size()), a, b, cfg);
            return p;
        }

        /**
         * @brief Stop used by gradient() to declare color stops.
         */
        template<Float T = double>
        struct Stop {
            const Color<T> *color = nullptr;
            T pos = T(0);
        };

        /**
         * @brief gradient(t, stops): evaluate color at param t in [0..1].
         * - Finds bracketing stops and linearly mixes with concentrations.
         */
        template<Float T = double>
        inline Color<T> gradient(T t, std::span<const Stop<T>> stops, const Config &cfg = {}) {
            t = detail::clamp01(t);

            const Stop<T> *a = nullptr;
            const Stop<T> *b = nullptr;

            for (const auto &s: stops) {
                if (!s.color) continue;
                if (s.pos <= t && (!a || s.pos > a->pos)) a = &s;
                if (s.pos >= t && (!b || s.pos < b->pos)) b = &s;
            }

            if (!a && b) return *b->color;
            if (!b && a) return *a->color;
            if (!a && !b) return Color<T>(RGB8{0, 0, 0}, cfg);

            if (a->pos == b->pos) return *a->color;

            const T factor = (t - a->pos) / (b->pos - a->pos);
            return mix<T>({MixItem<T>{a->color, T(1) - factor}, MixItem<T>{b->color, factor}}, cfg);
        }

        // ------------------------------------------------------------
        // Rendering wrappers
        // ------------------------------------------------------------
        /**
         * @brief mix two RGB8 via spectral mixing and return RGB8 in-gamut mapped result.
         *
         * This is intended for use in drawing pipelines where colors are available as 8-bit values.
         */
        inline RGB8 mix_rgb8(RGB8 a, RGB8 b, double t, const Config &cfg = {}) {
            Color<double> ca(a, cfg);
            Color<double> cb(b, cfg);
            // Match JS palette behavior: mix([a, 1-t], [b, t]) with concentration model.
            const double fa = 1.0 - detail::clamp01(t);
            const double fb = detail::clamp01(t);
            auto out = mix<double>({MixItem<double>{&ca, fa}, MixItem<double>{&cb, fb}}, cfg);
            return out.toRGB8(true);
        }

        inline void mix_rgb8_batch(std::span<const RGB8> a, std::span<const RGB8> b, std::span<RGB8> out, double t,
                                   const Config &cfg = {}) {
            const std::size_t n = std::min({a.size(), b.size(), out.size()});
            for (std::size_t i = 0; i < n; ++i) out[i] = mix_rgb8(a[i], b[i], t, cfg);
        }
    } // namespace km38
} // namespace akruti::spectral

#endif // AKRUTI_SPECTRAL_KM_HPP
