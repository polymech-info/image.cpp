#pragma once
//
// image_quality — cheap, non-LLM still-image stats for junk / accident detection.
// Quality and fingerprints share one decode.
//
// Pipeline (pure after the RGB thumbnail exists):
//   RGB bytes → extract_pixel_features → derive_histogram/color
//            → optional extract_edge_features → classify_junk → ImageQuality
//
// libvips is used only to decode a small sRGB thumbnail. Callers decide
// thresholds and actions. Not a boolean "is junk".
//
#include "export.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace media {

enum class ImageJunkKind {
    Dark   = 0,
    Bright = 1,
    Blur   = 2,
    Flat   = 3,
    Tiny   = 4,
};

struct EdgeOptions {
    /// `dx + dy` above this counts as an edge pixel (luma units, 0..510).
    int threshold = 24;
};

struct ImageQualityOptions {
    /// Longest side of the analysis frame. Original dimensions are still recorded.
    int analyze_max_side = 128;

    /// Kept for API compatibility. Classification uses fraction / entropy / edges.
    double dark_luma_hi = 40.0;
    double bright_luma_lo = 215.0;
    double blur_sharpness_hi = 80.0;
    double flat_luma_std_hi = 25.0;
    int    tiny_min_side = 128;

    int dark_bin   = 16;
    int bright_bin = 240;

    double dark_fraction_lo   = 0.82;
    double bright_fraction_lo = 0.82;
    double entropy_content_lo = 1.6;
    double entropy_skip_hi    = 0.35;
    double dynamic_range_flat_hi = 0.18;
    double dynamic_range_skip_hi = 0.04;
    double edge_density_content_lo = 0.06;
    double edge_energy_blur_hi = 12.0;
    /// p90 of |dx|+|dy|. Sharp subject on a flat wall still has a high tail.
    double edge_energy_p90_blur_hi = 40.0;
    /// p99 of |dx|+|dy|. Hard veto for blur: screenshots / sharp islands stay out.
    double edge_energy_p99_blur_hi = 88.0;
    /// Energy-weighted 8-bin orientation peak (0..1). Motion smear concentrates.
    double edge_orient_blur_lo = 0.20;
    double edge_orient_blur_span = 0.11;
    double skip_dark_fraction   = 0.97;
    double skip_bright_fraction = 0.97;
    EdgeOptions edges;
};

struct PixelFeatures {
    std::uint64_t pixel_count = 0;
    double luma_sum    = 0;
    double luma_sum_sq = 0;
    std::uint64_t dark_count   = 0;
    std::uint64_t bright_count = 0;
    double rg_sum    = 0;
    double rg_sum_sq = 0;
    double yb_sum    = 0;
    double yb_sum_sq = 0;
    std::array<std::uint32_t, 256> luma_hist{};
    /// 8 hue-ish octants × 3 saturation classes. Filled during the RGB scan.
    std::array<std::uint32_t, 24> color_hist{};
};

/// One libvips thumbnail, materialized as sRGB uchar. Shared by quality + fingerprint.
struct AnalysisRgbFrame {
    bool ok = false;
    std::string error;
    std::vector<std::uint8_t> rgb;
    int width  = 0;
    int height = 0;
    int bands  = 0;
    int source_width  = 0;
    int source_height = 0;
    std::uint64_t file_bytes = 0;
};

struct HistogramFeatures {
    double luma_mean  = 0;
    double luma_std   = 0;
    double pct_dark   = 0;
    double pct_bright = 0;
    double entropy    = 0;
    std::uint8_t p01 = 0;
    std::uint8_t p05 = 0;
    std::uint8_t p95 = 0;
    std::uint8_t p99 = 0;
    double dynamic_range = 0;
};

struct EdgeFeatures {
    double edge_energy  = 0;
    double edge_density = 0;
    double edge_energy_p90 = 0;
    double edge_energy_p99 = 0;
    /// Max fraction of gradient energy in one of 8 unsigned-orientation bins.
    double edge_orient_max = 0;
};

struct ColorFeatures {
    double colorfulness = 0;
};

struct ImageFeatures {
    HistogramFeatures histogram;
    EdgeFeatures      edges;
    ColorFeatures     color;
    int width  = 0;
    int height = 0;
    int analyzed_width  = 0;
    int analyzed_height = 0;
    std::uint64_t file_bytes = 0;
    double bytes_per_pixel = 0;
};

struct JunkScores {
    double dark   = 0;
    double bright = 0;
    double blur   = 0;
    double flat   = 0;
    double tiny   = 0;
    double hint   = 0;
    std::string primary_kind;
};

struct ImageQuality {
    bool        ok = false;
    std::string error;

    int         width  = 0;
    int         height = 0;
    int         analyzed_width  = 0;
    int         analyzed_height = 0;
    std::uint64_t file_bytes = 0;
    double      bytes_per_pixel = 0.0;

    double luma_mean    = 0.0;
    double luma_std     = 0.0;
    double pct_dark     = 0.0;
    double pct_bright   = 0.0;
    double sharpness    = 0.0; // edge energy (mean |dx|+|dy|); JSON key kept
    double colorfulness = 0.0;
    double entropy      = 0.0;
    double dynamic_range = 0.0;
    double edge_density  = 0.0;
    double edge_energy   = 0.0;
    double edge_energy_p90 = 0.0;
    double edge_energy_p99 = 0.0;
    double edge_orient_max = 0.0;
    std::uint8_t p05 = 0;
    std::uint8_t p95 = 0;

    double junk_dark   = 0.0;
    double junk_bright = 0.0;
    double junk_blur   = 0.0;
    double junk_flat   = 0.0;
    double junk_tiny   = 0.0;
    double junk_hint   = 0.0;
    std::string primary_kind;
};

[[nodiscard]] inline std::uint8_t fast_luma(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    return static_cast<std::uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
}

[[nodiscard]] inline std::uint8_t luma_at(std::span<const std::uint8_t> rgb,
                                          int x,
                                          int y,
                                          int width,
                                          int bands) noexcept
{
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                           + static_cast<std::size_t>(x))
        * static_cast<std::size_t>(bands);
    if (bands < 3)
        return rgb[i];
    return fast_luma(rgb[i], rgb[i + 1], rgb[i + 2]);
}

/// Same |dx|+|dy| used by edge metrics and the edge hash.
[[nodiscard]] inline int gradient_at(std::span<const std::uint8_t> rgb,
                                     int x,
                                     int y,
                                     int width,
                                     int height,
                                     int bands) noexcept
{
    if (x < 0 || y < 0 || x >= width - 1 || y >= height - 1 || bands <= 0)
        return 0;
    const int y00 = static_cast<int>(luma_at(rgb, x, y, width, bands));
    const int dx = std::abs(static_cast<int>(luma_at(rgb, x + 1, y, width, bands)) - y00);
    const int dy = std::abs(static_cast<int>(luma_at(rgb, x, y + 1, width, bands)) - y00);
    return dx + dy;
}

[[nodiscard]] inline double clamp01(double v) noexcept
{
    if (v < 0.0)
        return 0.0;
    if (v > 1.0)
        return 1.0;
    return v;
}

[[nodiscard]] PM_IMAGE_API double score_below(double value, double threshold) noexcept;
[[nodiscard]] PM_IMAGE_API double score_above(double value, double threshold, double span) noexcept;
[[nodiscard]] PM_IMAGE_API double combine_and(double a, double b) noexcept;
[[nodiscard]] PM_IMAGE_API double combine_and(double a, double b, double c) noexcept;
[[nodiscard]] PM_IMAGE_API double combine_or(double a, double b) noexcept;

[[nodiscard]] PM_IMAGE_API std::uint8_t histogram_percentile(const PixelFeatures& pixels,
                                                            double p) noexcept;

[[nodiscard]] PM_IMAGE_API PixelFeatures extract_pixel_features(
    std::span<const std::uint8_t> rgb,
    int width,
    int height,
    int bands,
    const ImageQualityOptions& opts);

[[nodiscard]] PM_IMAGE_API HistogramFeatures derive_histogram_features(const PixelFeatures& pixels);
[[nodiscard]] PM_IMAGE_API ColorFeatures derive_color_features(const PixelFeatures& pixels);

[[nodiscard]] PM_IMAGE_API EdgeFeatures extract_edge_features(
    std::span<const std::uint8_t> rgb,
    int width,
    int height,
    int bands,
    const EdgeOptions& opts);

[[nodiscard]] PM_IMAGE_API bool needs_edge_analysis(const HistogramFeatures& histogram,
                                                    const ImageQualityOptions& opts) noexcept;

[[nodiscard]] PM_IMAGE_API JunkScores classify_junk(const ImageFeatures& features,
                                                    const ImageQualityOptions& opts) noexcept;

[[nodiscard]] PM_IMAGE_API ImageQuality image_quality_from_features(const ImageFeatures& features,
                                                                   const JunkScores& scores);

[[nodiscard]] PM_IMAGE_API ImageQuality analyze_image_quality_from_rgb(
    std::span<const std::uint8_t> rgb,
    int width,
    int height,
    int bands,
    const ImageQualityOptions& opts = {},
    int source_width = 0,
    int source_height = 0,
    std::uint64_t file_bytes = 0);

PM_IMAGE_API const char* image_junk_kind_name(ImageJunkKind kind) noexcept;
PM_IMAGE_API std::vector<ImageJunkKind> parse_image_junk_kinds(const std::string& text,
                                                               std::string* error = nullptr);
PM_IMAGE_API double image_junk_score(const ImageQuality& q, ImageJunkKind kind) noexcept;
PM_IMAGE_API double image_junk_hint_for(const ImageQuality& q,
                                        const std::vector<ImageJunkKind>& kinds,
                                        ImageJunkKind* primary = nullptr) noexcept;

PM_IMAGE_API AnalysisRgbFrame load_analysis_rgb(const std::string& path, int analyze_max_side);

PM_IMAGE_API ImageQuality analyze_image_quality(const std::string& path,
                                                const ImageQualityOptions& opts = {});

PM_IMAGE_API nlohmann::json image_quality_to_json(const ImageQuality& q);

/// Set one option from `key=value` or JSON (`snake` or `camel`). Unknown key → false.
PM_IMAGE_API bool apply_image_quality_option(const std::string& key,
                                             const std::string& value,
                                             ImageQualityOptions& opts,
                                             std::string* error = nullptr);
PM_IMAGE_API void apply_image_quality_options_from_json(const nlohmann::json& j,
                                                        ImageQualityOptions& opts);

} // namespace media
