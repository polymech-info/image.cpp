#include "images/image_fingerprint.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace media {
namespace {

constexpr double k_pi = 3.14159265358979323846;

const std::array<std::array<double, 8>, 32>& dct_cos_32x8()
{
    static const auto table = [] {
        std::array<std::array<double, 8>, 32> t{};
        for (int i = 0; i < 32; ++i) {
            for (int u = 0; u < 8; ++u)
                t[static_cast<std::size_t>(i)][static_cast<std::size_t>(u)] =
                    std::cos(k_pi * (static_cast<double>(i) + 0.5) * static_cast<double>(u) / 32.0);
        }
        return t;
    }();
    return table;
}

std::uint8_t box_luma(std::span<const std::uint8_t> rgb,
                      int width,
                      int height,
                      int bands,
                      int col,
                      int cols,
                      int row,
                      int rows) noexcept
{
    if (width <= 0 || height <= 0 || cols <= 0 || rows <= 0)
        return 0;
    const int x0 = col * width / cols;
    const int x1 = std::max(x0 + 1, (col + 1) * width / cols);
    const int y0 = row * height / rows;
    const int y1 = std::max(y0 + 1, (row + 1) * height / rows);
    std::uint32_t sum = 0;
    std::uint32_t n = 0;
    for (int y = y0; y < y1 && y < height; ++y) {
        for (int x = x0; x < x1 && x < width; ++x) {
            sum += luma_at(rgb, x, y, width, bands);
            ++n;
        }
    }
    return n ? static_cast<std::uint8_t>(sum / n) : 0;
}

template <std::size_t N>
std::array<std::uint16_t, N> normalize_hist(const std::uint32_t* counts, std::size_t bins) noexcept
{
    std::array<std::uint16_t, N> out{};
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < bins; ++i)
        total += counts[i];
    if (total == 0)
        return out;
    std::uint32_t used = 0;
    std::size_t last = 0;
    for (std::size_t i = 0; i < bins; ++i) {
        if (counts[i] == 0)
            continue;
        last = i;
        const auto v = static_cast<std::uint16_t>((counts[i] * 65535ull) / total);
        out[i] = v;
        used += v;
    }
    if (used < 65535)
        out[last] = static_cast<std::uint16_t>(out[last] + (65535 - used));
    return out;
}

double hist_l1(const std::uint16_t* a, const std::uint16_t* b, std::size_t n) noexcept
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < n; ++i)
        sum += static_cast<std::uint32_t>(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
    return static_cast<double>(sum) / (2.0 * 65535.0);
}

double aspect_of(int w, int h) noexcept
{
    if (w <= 0 || h <= 0)
        return 0;
    return static_cast<double>(w) / static_cast<double>(h);
}

double aspect_delta(double a, double b) noexcept
{
    if (a <= 0.0 || b <= 0.0)
        return 1.0;
    const double lo = std::min(a, b);
    const double hi = std::max(a, b);
    return (hi - lo) / hi;
}

std::string hex16(std::uint64_t h)
{
    std::ostringstream o;
    o << std::hex << std::setfill('0') << std::setw(16) << h;
    return o.str();
}

} // namespace

std::uint64_t compute_dhash(std::span<const std::uint8_t> rgb, int width, int height, int bands)
{
    if (width <= 0 || height <= 0 || bands <= 0)
        return 0;
    std::uint8_t grid[8][9]{};
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 9; ++col)
            grid[row][col] = box_luma(rgb, width, height, bands, col, 9, row, 8);
    }
    std::uint64_t h = 0;
    int bit = 0;
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            if (grid[row][col] > grid[row][col + 1])
                h |= (1ull << bit);
            ++bit;
        }
    }
    return h;
}

std::uint64_t compute_phash(std::span<const std::uint8_t> rgb, int width, int height, int bands)
{
    if (width <= 0 || height <= 0 || bands <= 0)
        return 0;

    double grid[32][32]{};
    for (int row = 0; row < 32; ++row) {
        for (int col = 0; col < 32; ++col)
            grid[row][col] = static_cast<double>(box_luma(rgb, width, height, bands, col, 32, row, 32));
    }

    const auto& cos = dct_cos_32x8();
    double coeff[8][8]{};
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            double sum = 0.0;
            for (int y = 0; y < 32; ++y) {
                const double cy = cos[static_cast<std::size_t>(y)][static_cast<std::size_t>(v)];
                for (int x = 0; x < 32; ++x)
                    sum += grid[y][x] * cos[static_cast<std::size_t>(x)][static_cast<std::size_t>(u)] * cy;
            }
            coeff[u][v] = sum;
        }
    }

    double ac[63]{};
    int n = 0;
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            if (u == 0 && v == 0)
                continue;
            ac[n++] = coeff[u][v];
        }
    }
    std::array<double, 63> sorted{};
    std::copy(ac, ac + 63, sorted.begin());
    std::nth_element(sorted.begin(), sorted.begin() + 31, sorted.end());
    const double med = sorted[31];

    std::uint64_t h = 0;
    int bit = 0;
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            if (u == 0 && v == 0)
                continue;
            if (coeff[u][v] > med)
                h |= (1ull << bit);
            ++bit;
        }
    }
    return h;
}

std::uint64_t compute_edge_hash(std::span<const std::uint8_t> rgb, int width, int height, int bands)
{
    if (width < 2 || height < 2 || bands <= 0)
        return 0;

    double acc[8][8]{};
    std::uint32_t cnt[8][8]{};
    for (int y = 0; y < height - 1; ++y) {
        const int cy = std::min(7, y * 8 / (height - 1));
        for (int x = 0; x < width - 1; ++x) {
            const int cx = std::min(7, x * 8 / (width - 1));
            acc[cy][cx] += static_cast<double>(gradient_at(rgb, x, y, width, height, bands));
            ++cnt[cy][cx];
        }
    }

    double energy[64]{};
    for (int i = 0; i < 64; ++i) {
        const int cy = i / 8;
        const int cx = i % 8;
        energy[i] = cnt[cy][cx] ? acc[cy][cx] / static_cast<double>(cnt[cy][cx]) : 0.0;
    }
    std::array<double, 64> sorted{};
    std::copy(energy, energy + 64, sorted.begin());
    std::nth_element(sorted.begin(), sorted.begin() + 32, sorted.end());
    const double med = sorted[32];

    std::uint64_t h = 0;
    for (int i = 0; i < 64; ++i) {
        if (energy[i] > med)
            h |= (1ull << i);
    }
    return h;
}

std::array<std::uint16_t, 16> compact_luma_histogram(const PixelFeatures& pixels)
{
    std::uint32_t bins[16]{};
    for (int i = 0; i < 256; ++i)
        bins[i / 16] += pixels.luma_hist[static_cast<std::size_t>(i)];
    return normalize_hist<16>(bins, 16);
}

std::array<std::uint16_t, 24> compact_color_histogram(const PixelFeatures& pixels)
{
    return normalize_hist<24>(pixels.color_hist.data(), 24);
}

ImageFingerprint derive_image_fingerprint(std::span<const std::uint8_t> rgb,
                                          int width,
                                          int height,
                                          int bands,
                                          const PixelFeatures& pixels,
                                          const HistogramFeatures& histogram,
                                          const ColorFeatures& color,
                                          const EdgeFeatures& edges,
                                          const FingerprintOptions& opts,
                                          int source_width,
                                          int source_height)
{
    ImageFingerprint fp;
    fp.source_width = source_width > 0 ? source_width : width;
    fp.source_height = source_height > 0 ? source_height : height;
    fp.aspect_ratio = aspect_of(fp.source_width, fp.source_height);
    fp.entropy = histogram.entropy;
    fp.edge_density = edges.edge_density;
    fp.colorfulness = color.colorfulness;
    fp.luma_hist = compact_luma_histogram(pixels);
    if (opts.compute_color_hist)
        fp.color_hist = compact_color_histogram(pixels);
    if (opts.compute_dhash)
        fp.dhash = compute_dhash(rgb, width, height, bands);
    if (opts.compute_phash)
        fp.phash = compute_phash(rgb, width, height, bands);
    if (opts.compute_edge_hash)
        fp.edge_hash = compute_edge_hash(rgb, width, height, bands);
    return fp;
}

FingerprintDistance compare_fingerprints(const ImageFingerprint& a,
                                         const ImageFingerprint& b,
                                         const FingerprintCompareOptions& opts) noexcept
{
    FingerprintDistance d;
    d.dhash = hamming_distance(a.dhash, b.dhash);
    d.phash = hamming_distance(a.phash, b.phash);
    d.edge_hash = hamming_distance(a.edge_hash, b.edge_hash);
    d.luma_hist = hist_l1(a.luma_hist.data(), b.luma_hist.data(), a.luma_hist.size());
    d.color_hist = hist_l1(a.color_hist.data(), b.color_hist.data(), a.color_hist.size());
    d.aspect_delta = aspect_delta(a.aspect_ratio, b.aspect_ratio);

    const double dhash_score = 1.0 - static_cast<double>(d.dhash) / 64.0;
    const double phash_score = 1.0 - static_cast<double>(d.phash) / 64.0;
    const double edge_score = 1.0 - static_cast<double>(d.edge_hash) / 64.0;
    const double luma_score = 1.0 - clamp01(d.luma_hist);
    const double color_score = 1.0 - clamp01(d.color_hist);

    double w_sum = 0.0;
    double acc = 0.0;
    auto add = [&](double w, double score) {
        if (w <= 0.0)
            return;
        w_sum += w;
        acc += w * score;
    };
    add(opts.dhash_weight, dhash_score);
    add(opts.phash_weight, phash_score);
    add(opts.edge_weight, edge_score);
    add(opts.color_weight, color_score);
    add(opts.luma_weight, luma_score);
    d.similarity = w_sum > 0.0 ? clamp01(acc / w_sum) : 0.0;
    if (d.aspect_delta > opts.max_aspect_delta)
        d.similarity *= 0.5;
    return d;
}

DuplicateKind classify_duplicate(const FingerprintDistance& distance, const DuplicateOptions& opts) noexcept
{
    const int ham = std::max(distance.dhash, distance.phash);
    if (distance.dhash <= opts.duplicate_hamming && distance.phash <= opts.duplicate_hamming
        && distance.similarity >= opts.duplicate_similarity)
        return DuplicateKind::Duplicate;
    if ((ham <= opts.near_hamming && distance.similarity >= opts.near_similarity)
        || (distance.dhash <= opts.near_hamming && distance.phash <= opts.near_hamming))
        return DuplicateKind::NearDuplicate;
    if (distance.edge_hash <= opts.near_hamming && distance.dhash <= opts.similar_hamming)
        return DuplicateKind::Similar;
    if (distance.similarity >= opts.similar_similarity && ham <= opts.similar_hamming)
        return DuplicateKind::Similar;
    return DuplicateKind::None;
}

const char* duplicate_kind_name(DuplicateKind kind) noexcept
{
    switch (kind) {
    case DuplicateKind::Similar:       return "similar";
    case DuplicateKind::NearDuplicate: return "near_duplicate";
    case DuplicateKind::Duplicate:     return "duplicate";
    case DuplicateKind::None:          return "none";
    }
    return "none";
}

nlohmann::json image_fingerprint_to_json(const ImageFingerprint& fp)
{
    return {
        {"dhash", hex16(fp.dhash)},
        {"phash", hex16(fp.phash)},
        {"edgeHash", hex16(fp.edge_hash)},
        {"lumaHist", fp.luma_hist},
        {"colorHist", fp.color_hist},
        {"aspectRatio", fp.aspect_ratio},
        {"entropy", fp.entropy},
        {"edgeDensity", fp.edge_density},
        {"colorfulness", fp.colorfulness},
        {"sourceWidth", fp.source_width},
        {"sourceHeight", fp.source_height},
    };
}

nlohmann::json fingerprint_distance_to_json(const FingerprintDistance& d)
{
    return {
        {"dhash", d.dhash},
        {"phash", d.phash},
        {"edgeHash", d.edge_hash},
        {"lumaHist", d.luma_hist},
        {"colorHist", d.color_hist},
        {"aspectDelta", d.aspect_delta},
        {"similarity", d.similarity},
    };
}

} // namespace media
