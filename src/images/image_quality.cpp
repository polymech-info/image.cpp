#include "image_quality.hpp"
#include "fs/path_sanitizer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <vips/vips.h>
#include "string/string_utils.hpp"

namespace media {
namespace {

namespace fs = std::filesystem;

std::once_flag g_vips_init;

void ensure_vips()
{
    std::call_once(g_vips_init, []() {
        if (vips_init("image_quality"))
            std::abort();
    });
}

std::string vips_err()
{
    const char* b = vips_error_buffer();
    std::string s = b ? b : "vips error";
    vips_error_clear();
    return s;
}

void unref_image(VipsImage*& img)
{
    if (!img)
        return;
    g_object_unref(img);
    img = nullptr;
}

bool to_uchar_rgb(VipsImage* in, VipsImage** out)
{
    *out = nullptr;
    VipsImage* cur = in;
    g_object_ref(cur);

    if (vips_image_get_bands(cur) > 3) {
        VipsImage* sliced = nullptr;
        if (vips_extract_band(cur, &sliced, 0, "n", 3, nullptr)) {
            unref_image(cur);
            return false;
        }
        unref_image(cur);
        cur = sliced;
    }

    if (vips_image_get_format(cur) != VIPS_FORMAT_UCHAR) {
        VipsImage* u8 = nullptr;
        if (vips_cast(cur, &u8, VIPS_FORMAT_UCHAR, nullptr)) {
            unref_image(cur);
            return false;
        }
        unref_image(cur);
        cur = u8;
    }

    *out = cur;
    return true;
}

bool write_u8(VipsImage* img, std::vector<std::uint8_t>& pixels, int& w, int& h, int& bands)
{
    w = vips_image_get_width(img);
    h = vips_image_get_height(img);
    bands = vips_image_get_bands(img);
    if (w <= 0 || h <= 0 || bands <= 0)
        return false;
    size_t sz = 0;
    void* mem = vips_image_write_to_memory(img, &sz);
    if (!mem)
        return false;
    pixels.assign(static_cast<const std::uint8_t*>(mem),
                  static_cast<const std::uint8_t*>(mem) + sz);
    g_free(mem);
    const std::size_t expect = static_cast<std::size_t>(w) * static_cast<std::size_t>(h)
        * static_cast<std::size_t>(bands);
    return pixels.size() >= expect;
}

int color_hist_bin(double r, double g, double b) noexcept
{
    const double rg = r - g;
    const double yb = 0.5 * (r + g) - b;
    const double chroma2 = rg * rg + yb * yb;
    int sat = 0;
    if (chroma2 >= 60.0 * 60.0)
        sat = 2;
    else if (chroma2 >= 20.0 * 20.0)
        sat = 1;

    const double arg = std::abs(rg);
    const double ayb = std::abs(yb);
    int hue = 0;
    if (arg >= ayb)
        hue = (rg >= 0.0) ? (yb >= 0.0 ? 0 : 7) : (yb >= 0.0 ? 3 : 4);
    else
        hue = (yb >= 0.0) ? (rg >= 0.0 ? 1 : 2) : (rg >= 0.0 ? 6 : 5);
    return hue * 3 + sat;
}

std::string trim_copy(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}


} // namespace

double score_below(double value, double threshold) noexcept
{
    if (threshold <= 0.0)
        return value <= 0.0 ? 1.0 : 0.0;
    if (value >= threshold)
        return 0.0;
    return clamp01((threshold - value) / threshold);
}

double score_above(double value, double threshold, double span) noexcept
{
    if (span <= 0.0)
        return value >= threshold ? 1.0 : 0.0;
    if (value <= threshold)
        return 0.0;
    return clamp01((value - threshold) / span);
}

double combine_and(double a, double b) noexcept
{
    return clamp01(a) * clamp01(b);
}

double combine_and(double a, double b, double c) noexcept
{
    return clamp01(a) * clamp01(b) * clamp01(c);
}

double combine_or(double a, double b) noexcept
{
    const double ia = 1.0 - clamp01(a);
    const double ib = 1.0 - clamp01(b);
    return clamp01(1.0 - ia * ib);
}

std::uint8_t histogram_percentile(const PixelFeatures& pixels, double p) noexcept
{
    if (pixels.pixel_count == 0)
        return 0;
    const double target = std::clamp(p, 0.0, 1.0) * static_cast<double>(pixels.pixel_count);
    if (target <= 0.0) {
        for (int i = 0; i < 256; ++i) {
            if (pixels.luma_hist[static_cast<std::size_t>(i)] != 0)
                return static_cast<std::uint8_t>(i);
        }
        return 0;
    }
    double cum = 0.0;
    for (int i = 0; i < 256; ++i) {
        cum += static_cast<double>(pixels.luma_hist[static_cast<std::size_t>(i)]);
        if (cum >= target)
            return static_cast<std::uint8_t>(i);
    }
    return 255;
}

PixelFeatures extract_pixel_features(std::span<const std::uint8_t> rgb,
                                     int width,
                                     int height,
                                     int bands,
                                     const ImageQualityOptions& opts)
{
    PixelFeatures out;
    if (width <= 0 || height <= 0 || bands <= 0)
        return out;
    const std::size_t need = static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
        * static_cast<std::size_t>(bands);
    if (rgb.size() < need)
        return out;

    const int dark_bin = std::clamp(opts.dark_bin, 0, 255);
    const int bright_bin = std::clamp(opts.bright_bin, 0, 255);
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t stride = static_cast<std::size_t>(bands);

    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = rgb.data() + i * stride;
        const std::uint8_t y = (bands >= 3) ? fast_luma(p[0], p[1], p[2]) : p[0];
        const double yd = static_cast<double>(y);
        out.luma_sum += yd;
        out.luma_sum_sq += yd * yd;
        ++out.luma_hist[y];
        if (y <= static_cast<std::uint8_t>(dark_bin))
            ++out.dark_count;
        if (y >= static_cast<std::uint8_t>(bright_bin))
            ++out.bright_count;
        if (bands >= 3) {
            const double r = static_cast<double>(p[0]);
            const double g = static_cast<double>(p[1]);
            const double b = static_cast<double>(p[2]);
            const double rg = r - g;
            const double yb = 0.5 * (r + g) - b;
            out.rg_sum += rg;
            out.rg_sum_sq += rg * rg;
            out.yb_sum += yb;
            out.yb_sum_sq += yb * yb;
            ++out.color_hist[static_cast<std::size_t>(color_hist_bin(r, g, b))];
        }
    }
    out.pixel_count = n;
    return out;
}

HistogramFeatures derive_histogram_features(const PixelFeatures& pixels)
{
    HistogramFeatures h;
    if (pixels.pixel_count == 0)
        return h;

    const double n = static_cast<double>(pixels.pixel_count);
    h.luma_mean = pixels.luma_sum / n;
    h.luma_std = std::sqrt(std::max(0.0, (pixels.luma_sum_sq / n) - (h.luma_mean * h.luma_mean)));
    h.pct_dark = static_cast<double>(pixels.dark_count) / n;
    h.pct_bright = static_cast<double>(pixels.bright_count) / n;
    h.p01 = histogram_percentile(pixels, 0.01);
    h.p05 = histogram_percentile(pixels, 0.05);
    h.p95 = histogram_percentile(pixels, 0.95);
    h.p99 = histogram_percentile(pixels, 0.99);
    h.dynamic_range = static_cast<double>(h.p99 - h.p01) / 255.0;

    double entropy = 0.0;
    for (std::uint32_t count : pixels.luma_hist) {
        if (count == 0)
            continue;
        const double p = static_cast<double>(count) / n;
        entropy -= p * (std::log(p) / std::log(2.0));
    }
    h.entropy = entropy;
    return h;
}

ColorFeatures derive_color_features(const PixelFeatures& pixels)
{
    ColorFeatures c;
    if (pixels.pixel_count == 0)
        return c;
    const double n = static_cast<double>(pixels.pixel_count);
    const double mean_rg = pixels.rg_sum / n;
    const double mean_yb = pixels.yb_sum / n;
    const double std_rg = std::sqrt(std::max(0.0, (pixels.rg_sum_sq / n) - (mean_rg * mean_rg)));
    const double std_yb = std::sqrt(std::max(0.0, (pixels.yb_sum_sq / n) - (mean_yb * mean_yb)));
    c.colorfulness = std::sqrt(std_rg * std_rg + std_yb * std_yb)
        + 0.3 * std::sqrt(mean_rg * mean_rg + mean_yb * mean_yb);
    return c;
}

EdgeFeatures extract_edge_features(std::span<const std::uint8_t> rgb,
                                   int width,
                                   int height,
                                   int bands,
                                   const EdgeOptions& opts)
{
    EdgeFeatures e;
    if (width < 2 || height < 2 || bands <= 0)
        return e;
    const std::size_t need = static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
        * static_cast<std::size_t>(bands);
    if (rgb.size() < need)
        return e;

    constexpr double k_pi = 3.14159265358979323846;
    const int thresh = std::max(0, opts.threshold);
    double energy = 0.0;
    std::uint64_t edges = 0;
    std::uint64_t count = 0;
    std::array<std::uint32_t, 64> hist{};
    std::array<double, 8> orient{};
    double orient_sum = 0.0;
    for (int y = 0; y < height - 1; ++y) {
        for (int x = 0; x < width - 1; ++x) {
            const int y00 = static_cast<int>(luma_at(rgb, x, y, width, bands));
            const int sdx = static_cast<int>(luma_at(rgb, x + 1, y, width, bands)) - y00;
            const int sdy = static_cast<int>(luma_at(rgb, x, y + 1, width, bands)) - y00;
            const int edge = std::abs(sdx) + std::abs(sdy);
            energy += static_cast<double>(edge);
            if (edge > thresh)
                ++edges;
            ++hist[static_cast<std::size_t>(std::min(63, edge >> 3))];
            ++count;
            if (edge >= 16) {
                double ang = std::atan2(static_cast<double>(sdy), static_cast<double>(sdx));
                if (ang < 0.0)
                    ang += k_pi;
                int bin = static_cast<int>(ang * (8.0 / k_pi));
                if (bin < 0)
                    bin = 0;
                if (bin > 7)
                    bin = 7;
                const double w = static_cast<double>(edge);
                orient[static_cast<std::size_t>(bin)] += w;
                orient_sum += w;
            }
        }
    }
    if (count == 0)
        return e;
    e.edge_energy = energy / static_cast<double>(count);
    e.edge_density = static_cast<double>(edges) / static_cast<double>(count);

    auto percentile = [&](double p) {
        const double target = p * static_cast<double>(count);
        double cum = 0.0;
        for (int i = 0; i < 64; ++i) {
            cum += static_cast<double>(hist[static_cast<std::size_t>(i)]);
            if (cum >= target)
                return static_cast<double>(i * 8 + 4);
        }
        return 508.0;
    };
    e.edge_energy_p90 = percentile(0.90);
    e.edge_energy_p99 = percentile(0.99);
    if (orient_sum > 0.0) {
        double peak = 0.0;
        for (double v : orient) {
            if (v > peak)
                peak = v;
        }
        e.edge_orient_max = peak / orient_sum;
    }
    return e;
}

bool needs_edge_analysis(const HistogramFeatures& histogram,
                         const ImageQualityOptions& opts) noexcept
{
    if (histogram.entropy > opts.entropy_skip_hi)
        return true;
    if (histogram.pct_dark >= opts.skip_dark_fraction)
        return false;
    if (histogram.pct_bright >= opts.skip_bright_fraction)
        return false;
    if (histogram.dynamic_range <= opts.dynamic_range_skip_hi)
        return false;
    return true;
}

JunkScores classify_junk(const ImageFeatures& features, const ImageQualityOptions& opts) noexcept
{
    JunkScores s;
    const HistogramFeatures& h = features.histogram;
    const EdgeFeatures& e = features.edges;

    const double low_entropy = score_below(h.entropy, opts.entropy_content_lo);
    const double low_range = score_below(h.dynamic_range, opts.dynamic_range_flat_hi);
    const double low_edges = score_below(e.edge_density, opts.edge_density_content_lo);
    const double dark_frac = score_above(h.pct_dark, opts.dark_fraction_lo, 1.0 - opts.dark_fraction_lo);
    const double bright_frac = score_above(h.pct_bright, opts.bright_fraction_lo,
                                           1.0 - opts.bright_fraction_lo);
    const double has_content = combine_or(
        score_above(h.entropy, opts.entropy_content_lo, 8.0 - opts.entropy_content_lo),
        score_above(h.dynamic_range, opts.dynamic_range_flat_hi, 1.0 - opts.dynamic_range_flat_hi));
    const double low_edge_energy = score_below(e.edge_energy, opts.edge_energy_blur_hi);
    const double low_edge_tail = score_below(e.edge_energy_p90, opts.edge_energy_p90_blur_hi);
    const double directional = score_above(e.edge_orient_max, opts.edge_orient_blur_lo,
                                           opts.edge_orient_blur_span);

    s.dark = combine_and(dark_frac, low_entropy, low_edges);
    s.bright = combine_and(bright_frac, low_entropy, low_edges);
    s.flat = combine_and(low_range, low_entropy, low_edges);
    if (e.edge_energy_p99 >= opts.edge_energy_p99_blur_hi) {
        s.blur = 0.0;
    } else {
        const double motion = combine_and(directional, has_content);
        const double oof = combine_and(low_edge_energy, low_edge_tail, has_content);
        s.blur = combine_or(motion, oof);
    }
    const int min_side = std::min(features.width, features.height);
    s.tiny = score_below(static_cast<double>(min_side),
                         static_cast<double>(std::max(1, opts.tiny_min_side)));

    ImageQuality tmp;
    tmp.junk_dark = s.dark;
    tmp.junk_bright = s.bright;
    tmp.junk_blur = s.blur;
    tmp.junk_flat = s.flat;
    tmp.junk_tiny = s.tiny;
    ImageJunkKind primary = ImageJunkKind::Dark;
    s.hint = image_junk_hint_for(tmp, {}, &primary);
    s.primary_kind = s.hint > 0.0 ? image_junk_kind_name(primary) : std::string{};
    return s;
}

ImageQuality image_quality_from_features(const ImageFeatures& features, const JunkScores& scores)
{
    ImageQuality q;
    q.ok = true;
    q.width = features.width;
    q.height = features.height;
    q.analyzed_width = features.analyzed_width;
    q.analyzed_height = features.analyzed_height;
    q.file_bytes = features.file_bytes;
    q.bytes_per_pixel = features.bytes_per_pixel;
    q.luma_mean = features.histogram.luma_mean;
    q.luma_std = features.histogram.luma_std;
    q.pct_dark = features.histogram.pct_dark;
    q.pct_bright = features.histogram.pct_bright;
    q.entropy = features.histogram.entropy;
    q.dynamic_range = features.histogram.dynamic_range;
    q.p05 = features.histogram.p05;
    q.p95 = features.histogram.p95;
    q.edge_energy = features.edges.edge_energy;
    q.edge_energy_p90 = features.edges.edge_energy_p90;
    q.edge_energy_p99 = features.edges.edge_energy_p99;
    q.edge_orient_max = features.edges.edge_orient_max;
    q.edge_density = features.edges.edge_density;
    q.sharpness = features.edges.edge_energy;
    q.colorfulness = features.color.colorfulness;
    q.junk_dark = scores.dark;
    q.junk_bright = scores.bright;
    q.junk_blur = scores.blur;
    q.junk_flat = scores.flat;
    q.junk_tiny = scores.tiny;
    q.junk_hint = scores.hint;
    q.primary_kind = scores.primary_kind;
    return q;
}

ImageQuality analyze_image_quality_from_rgb(std::span<const std::uint8_t> rgb,
                                            int width,
                                            int height,
                                            int bands,
                                            const ImageQualityOptions& opts,
                                            int source_width,
                                            int source_height,
                                            std::uint64_t file_bytes)
{
    ImageQuality q;
    const PixelFeatures pixels = extract_pixel_features(rgb, width, height, bands, opts);
    if (pixels.pixel_count == 0) {
        q.error = "image_quality: empty or invalid RGB buffer";
        return q;
    }

    ImageFeatures features;
    features.histogram = derive_histogram_features(pixels);
    features.color = derive_color_features(pixels);
    features.analyzed_width = width;
    features.analyzed_height = height;
    features.width = source_width > 0 ? source_width : width;
    features.height = source_height > 0 ? source_height : height;
    features.file_bytes = file_bytes;
    if (features.width > 0 && features.height > 0)
        features.bytes_per_pixel = static_cast<double>(file_bytes)
            / (static_cast<double>(features.width) * static_cast<double>(features.height));

    const bool tiny = std::min(features.width, features.height) < std::max(1, opts.tiny_min_side);
    if (!tiny && needs_edge_analysis(features.histogram, opts))
        features.edges = extract_edge_features(rgb, width, height, bands, opts.edges);

    return image_quality_from_features(features, classify_junk(features, opts));
}

const char* image_junk_kind_name(ImageJunkKind kind) noexcept
{
    switch (kind) {
    case ImageJunkKind::Dark:   return "dark";
    case ImageJunkKind::Bright: return "bright";
    case ImageJunkKind::Blur:   return "blur";
    case ImageJunkKind::Flat:   return "flat";
    case ImageJunkKind::Tiny:   return "tiny";
    }
    return "dark";
}

std::vector<ImageJunkKind> parse_image_junk_kinds(const std::string& text, std::string* error)
{
    if (error)
        error->clear();
    std::string t = media::strutil::lower_ascii(trim_copy(text));
    if (t.empty() || t == "all") {
        return {
            ImageJunkKind::Dark,
            ImageJunkKind::Bright,
            ImageJunkKind::Blur,
            ImageJunkKind::Flat,
            ImageJunkKind::Tiny,
        };
    }

    std::vector<ImageJunkKind> out;
    std::string token;
    auto flush = [&](std::string raw) -> bool {
        raw = media::strutil::lower_ascii(trim_copy(std::move(raw)));
        if (raw.empty())
            return true;
        if (raw == "dark")
            out.push_back(ImageJunkKind::Dark);
        else if (raw == "bright" || raw == "blown" || raw == "white")
            out.push_back(ImageJunkKind::Bright);
        else if (raw == "blur" || raw == "blurred" || raw == "soft")
            out.push_back(ImageJunkKind::Blur);
        else if (raw == "flat" || raw == "empty")
            out.push_back(ImageJunkKind::Flat);
        else if (raw == "tiny" || raw == "small")
            out.push_back(ImageJunkKind::Tiny);
        else {
            if (error)
                *error = "unknown junk kind: " + raw;
            return false;
        }
        return true;
    };

    for (char c : t) {
        if (c == ',' || c == ';' || c == '|') {
            if (!flush(token))
                return {};
            token.clear();
        } else {
            token.push_back(c);
        }
    }
    if (!flush(token))
        return {};
    if (out.empty()) {
        if (error)
            *error = "no junk kinds given";
        return {};
    }
    return out;
}

double image_junk_score(const ImageQuality& q, ImageJunkKind kind) noexcept
{
    switch (kind) {
    case ImageJunkKind::Dark:   return q.junk_dark;
    case ImageJunkKind::Bright: return q.junk_bright;
    case ImageJunkKind::Blur:   return q.junk_blur;
    case ImageJunkKind::Flat:   return q.junk_flat;
    case ImageJunkKind::Tiny:   return q.junk_tiny;
    }
    return 0.0;
}

double image_junk_hint_for(const ImageQuality& q,
                           const std::vector<ImageJunkKind>& kinds,
                           ImageJunkKind* primary) noexcept
{
    const ImageJunkKind all[] = {
        ImageJunkKind::Dark,
        ImageJunkKind::Bright,
        ImageJunkKind::Blur,
        ImageJunkKind::Flat,
        ImageJunkKind::Tiny,
    };
    const ImageJunkKind* begin = kinds.empty() ? all : kinds.data();
    const ImageJunkKind* end = kinds.empty() ? all + 5 : kinds.data() + kinds.size();

    double best = -1.0;
    ImageJunkKind best_kind = ImageJunkKind::Dark;
    for (const ImageJunkKind* it = begin; it != end; ++it) {
        const double s = image_junk_score(q, *it);
        if (s > best) {
            best = s;
            best_kind = *it;
        }
    }
    if (primary)
        *primary = best_kind;
    return best < 0.0 ? 0.0 : best;
}

AnalysisRgbFrame load_analysis_rgb(const std::string& path, int analyze_max_side)
{
    AnalysisRgbFrame out;
    if (path.empty()) {
        out.error = "image_quality: empty path";
        return out;
    }

    ensure_vips();

    std::error_code ec;
    const fs::path file = media::path::path_from_utf8(path);
    if (!fs::is_regular_file(file, ec) || ec) {
        out.error = "image_quality: not a regular file";
        return out;
    }
    out.file_bytes = [&] {
        const auto n = fs::file_size(file, ec);
        return ec ? 0 : static_cast<std::uint64_t>(n);
    }();

    VipsImage* header = vips_image_new_from_file(path.c_str(), "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
    if (!header) {
        out.error = "image_quality: " + vips_err();
        return out;
    }
    out.source_width = vips_image_get_width(header);
    out.source_height = vips_image_get_height(header);
    unref_image(header);

    const int max_side = std::max(16, analyze_max_side);
    VipsImage* thumb = nullptr;
    if (vips_thumbnail(path.c_str(), &thumb, max_side, "size", VIPS_SIZE_DOWN, nullptr)) {
        out.error = "image_quality: " + vips_err();
        return out;
    }

    VipsImage* rgb_cs = nullptr;
    if (vips_colourspace(thumb, &rgb_cs, VIPS_INTERPRETATION_sRGB, nullptr)) {
        unref_image(thumb);
        out.error = "image_quality: " + vips_err();
        return out;
    }
    unref_image(thumb);

    VipsImage* rgb = nullptr;
    if (!to_uchar_rgb(rgb_cs, &rgb)) {
        unref_image(rgb_cs);
        out.error = "image_quality: " + vips_err();
        return out;
    }
    unref_image(rgb_cs);

    if (!write_u8(rgb, out.rgb, out.width, out.height, out.bands)) {
        unref_image(rgb);
        out.error = "image_quality: failed to read analysis pixels";
        return out;
    }
    unref_image(rgb);
    out.ok = true;
    return out;
}

ImageQuality analyze_image_quality(const std::string& path, const ImageQualityOptions& opts)
{
    const AnalysisRgbFrame frame = load_analysis_rgb(path, opts.analyze_max_side);
    if (!frame.ok) {
        ImageQuality q;
        q.error = frame.error;
        return q;
    }
    return analyze_image_quality_from_rgb(frame.rgb, frame.width, frame.height, frame.bands,
                                          opts, frame.source_width, frame.source_height,
                                          frame.file_bytes);
}

nlohmann::json image_quality_to_json(const ImageQuality& q)
{
    return {
        {"ok", q.ok},
        {"error", q.error},
        {"width", q.width},
        {"height", q.height},
        {"analyzedWidth", q.analyzed_width},
        {"analyzedHeight", q.analyzed_height},
        {"fileBytes", q.file_bytes},
        {"bytesPerPixel", q.bytes_per_pixel},
        {"lumaMean", q.luma_mean},
        {"lumaStd", q.luma_std},
        {"pctDark", q.pct_dark},
        {"pctBright", q.pct_bright},
        {"sharpness", q.sharpness},
        {"colorfulness", q.colorfulness},
        {"entropy", q.entropy},
        {"dynamicRange", q.dynamic_range},
        {"edgeDensity", q.edge_density},
        {"edgeEnergy", q.edge_energy},
        {"edgeEnergyP90", q.edge_energy_p90},
        {"edgeEnergyP99", q.edge_energy_p99},
        {"edgeOrientMax", q.edge_orient_max},
        {"p05", q.p05},
        {"p95", q.p95},
        {"junk", {
            {"dark", q.junk_dark},
            {"bright", q.junk_bright},
            {"blur", q.junk_blur},
            {"flat", q.junk_flat},
            {"tiny", q.junk_tiny},
            {"hint", q.junk_hint},
            {"primary", q.primary_kind},
        }},
    };
}

namespace {

std::string canon_opt_key(std::string k)
{
    k = media::strutil::lower_ascii(trim_copy(std::move(k)));
    std::string out;
    out.reserve(k.size());
    for (char c : k) {
        if (c != '_' && c != '-')
            out.push_back(c);
    }
    return out;
}

bool parse_opt_double(const std::string& value, double& out, std::string* error)
{
    try {
        size_t n = 0;
        out = std::stod(value, &n);
        if (n == value.size())
            return true;
    } catch (...) {
    }
    if (error)
        *error = "not a number: " + value;
    return false;
}

bool parse_opt_int(const std::string& value, int& out, std::string* error)
{
    try {
        size_t n = 0;
        out = std::stoi(value, &n);
        if (n == value.size())
            return true;
    } catch (...) {
    }
    if (error)
        *error = "not an integer: " + value;
    return false;
}

} // namespace

bool apply_image_quality_option(const std::string& key,
                                const std::string& value,
                                ImageQualityOptions& opts,
                                std::string* error)
{
    if (error)
        error->clear();
    const std::string k = canon_opt_key(key);
    auto set_d = [&](double& dst) {
        return parse_opt_double(value, dst, error);
    };
    auto set_i = [&](int& dst) {
        return parse_opt_int(value, dst, error);
    };

    if (k == "analyzemaxside") return set_i(opts.analyze_max_side);
    if (k == "tinyminside") return set_i(opts.tiny_min_side);
    if (k == "darklumahi") return set_d(opts.dark_luma_hi);
    if (k == "brightlumalo") return set_d(opts.bright_luma_lo);
    if (k == "blursharpnesshi") return set_d(opts.blur_sharpness_hi);
    if (k == "flatlumastdhi") return set_d(opts.flat_luma_std_hi);
    if (k == "darkbin") return set_i(opts.dark_bin);
    if (k == "brightbin") return set_i(opts.bright_bin);
    if (k == "darkfractionlo" || k == "darkfraction") return set_d(opts.dark_fraction_lo);
    if (k == "brightfractionlo" || k == "brightfraction") return set_d(opts.bright_fraction_lo);
    if (k == "entropycontentlo" || k == "entropycontent") return set_d(opts.entropy_content_lo);
    if (k == "entropyskiphi") return set_d(opts.entropy_skip_hi);
    if (k == "dynamicrangeflathi") return set_d(opts.dynamic_range_flat_hi);
    if (k == "dynamicrangeskiphi") return set_d(opts.dynamic_range_skip_hi);
    if (k == "edgedensitycontentlo" || k == "edgedensitylo") return set_d(opts.edge_density_content_lo);
    if (k == "edgeenergyblurhi" || k == "edgeenergyblur") return set_d(opts.edge_energy_blur_hi);
    if (k == "edgeenergyp90blurhi" || k == "edgeenergyp90blur") return set_d(opts.edge_energy_p90_blur_hi);
    if (k == "edgeenergyp99blurhi" || k == "edgeenergyp99blur") return set_d(opts.edge_energy_p99_blur_hi);
    if (k == "edgeorientblurlo" || k == "edgeorientblur") return set_d(opts.edge_orient_blur_lo);
    if (k == "edgeorientblurspan") return set_d(opts.edge_orient_blur_span);
    if (k == "skipdarkfraction") return set_d(opts.skip_dark_fraction);
    if (k == "skipbrightfraction") return set_d(opts.skip_bright_fraction);
    if (k == "edgethreshold" || k == "edgesthreshold") return set_i(opts.edges.threshold);

    if (error)
        *error = "unknown image quality option: " + key;
    return false;
}

void apply_image_quality_options_from_json(const nlohmann::json& j, ImageQualityOptions& opts)
{
    if (!j.is_object())
        return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_null())
            continue;
        std::string value;
        if (it.value().is_string())
            value = it.value().get<std::string>();
        else if (it.value().is_number() || it.value().is_boolean())
            value = it.value().dump();
        else
            continue;
        apply_image_quality_option(it.key(), value, opts, nullptr);
    }
}

} // namespace media
