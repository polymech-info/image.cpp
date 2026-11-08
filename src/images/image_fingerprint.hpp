#pragma once
//
// Image fingerprints — pure distances on the same RGB thumbnail as quality.
// Grouping, indexes, and delete policy live outside this module.
//
#include "images/image_quality.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>

#include <nlohmann/json.hpp>

namespace media {

struct ImageFingerprint {
    std::uint64_t dhash = 0;
    std::uint64_t phash = 0;
    std::uint64_t edge_hash = 0;

    std::array<std::uint16_t, 16> luma_hist{};
    std::array<std::uint16_t, 24> color_hist{};

    double aspect_ratio = 0;
    double entropy = 0;
    double edge_density = 0;
    double colorfulness = 0;

    int source_width = 0;
    int source_height = 0;
};

struct FingerprintOptions {
    bool compute_dhash = true;
    bool compute_phash = true;
    bool compute_edge_hash = true;
    bool compute_color_hist = true;
};

struct FingerprintCompareOptions {
    double dhash_weight = 0.30;
    double phash_weight = 0.35;
    double edge_weight = 0.20;
    double color_weight = 0.10;
    double luma_weight = 0.05;
    double max_aspect_delta = 0.08;
};

struct FingerprintDistance {
    int dhash = 64;
    int phash = 64;
    int edge_hash = 64;

    double luma_hist = 1.0;
    double color_hist = 1.0;
    double aspect_delta = 1.0;

    double similarity = 0.0;
};

enum class DuplicateKind {
    None = 0,
    Similar,
    NearDuplicate,
    Duplicate,
};

struct DuplicateOptions {
    int duplicate_hamming = 3;
    int near_hamming = 8;
    int similar_hamming = 14;
    double duplicate_similarity = 0.90;
    double near_similarity = 0.75;
    double similar_similarity = 0.55;
};

[[nodiscard]] inline int hamming_distance(std::uint64_t a, std::uint64_t b) noexcept
{
    return std::popcount(a ^ b);
}

[[nodiscard]] PM_IMAGE_API std::uint64_t compute_dhash(
    std::span<const std::uint8_t> rgb, int width, int height, int bands);

[[nodiscard]] PM_IMAGE_API std::uint64_t compute_phash(
    std::span<const std::uint8_t> rgb, int width, int height, int bands);

[[nodiscard]] PM_IMAGE_API std::uint64_t compute_edge_hash(
    std::span<const std::uint8_t> rgb, int width, int height, int bands);

[[nodiscard]] PM_IMAGE_API std::array<std::uint16_t, 16>
compact_luma_histogram(const PixelFeatures& pixels);

[[nodiscard]] PM_IMAGE_API std::array<std::uint16_t, 24>
compact_color_histogram(const PixelFeatures& pixels);

[[nodiscard]] PM_IMAGE_API ImageFingerprint derive_image_fingerprint(
    std::span<const std::uint8_t> rgb,
    int width,
    int height,
    int bands,
    const PixelFeatures& pixels,
    const HistogramFeatures& histogram,
    const ColorFeatures& color,
    const EdgeFeatures& edges,
    const FingerprintOptions& opts = {},
    int source_width = 0,
    int source_height = 0);

[[nodiscard]] PM_IMAGE_API FingerprintDistance compare_fingerprints(
    const ImageFingerprint& a,
    const ImageFingerprint& b,
    const FingerprintCompareOptions& opts = {}) noexcept;

[[nodiscard]] PM_IMAGE_API DuplicateKind classify_duplicate(
    const FingerprintDistance& distance,
    const DuplicateOptions& opts = {}) noexcept;

PM_IMAGE_API const char* duplicate_kind_name(DuplicateKind kind) noexcept;

PM_IMAGE_API nlohmann::json image_fingerprint_to_json(const ImageFingerprint& fp);
PM_IMAGE_API nlohmann::json fingerprint_distance_to_json(const FingerprintDistance& d);

} // namespace media
