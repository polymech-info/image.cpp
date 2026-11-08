#pragma once
//
// Shared still-image analysis. One libvips thumbnail, then pure stages.
// Quality / junk and fingerprint / distances share the same RGB buffer.
//
#include "images/image_fingerprint.hpp"
#include "images/image_quality.hpp"

namespace media {

struct ImageAnalysisOptions {
    ImageQualityOptions quality;
    FingerprintOptions fingerprint;
    bool compute_quality = true;
    bool compute_fingerprint = false;
};

struct ImageAnalysis {
    bool ok = false;
    std::string error;
    ImageQuality quality;
    ImageFingerprint fingerprint;
};

PM_IMAGE_API ImageAnalysis analyze_image(const std::string& path,
                                         const ImageAnalysisOptions& opts = {});

PM_IMAGE_API ImageAnalysis analyze_image_from_rgb(
    std::span<const std::uint8_t> rgb,
    int width,
    int height,
    int bands,
    const ImageAnalysisOptions& opts = {},
    int source_width = 0,
    int source_height = 0,
    std::uint64_t file_bytes = 0);

} // namespace media
