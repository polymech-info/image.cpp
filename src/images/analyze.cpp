#include "images/analyze.hpp"

#include <algorithm>

namespace media {

ImageAnalysis analyze_image_from_rgb(std::span<const std::uint8_t> rgb,
                                     int width,
                                     int height,
                                     int bands,
                                     const ImageAnalysisOptions& opts,
                                     int source_width,
                                     int source_height,
                                     std::uint64_t file_bytes)
{
    ImageAnalysis out;
    if (!opts.compute_quality && !opts.compute_fingerprint) {
        out.ok = true;
        out.quality.ok = true;
        return out;
    }

    const PixelFeatures pixels = extract_pixel_features(rgb, width, height, bands, opts.quality);
    if (pixels.pixel_count == 0) {
        out.error = "image_analyze: empty or invalid RGB buffer";
        out.quality.error = out.error;
        return out;
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

    const bool tiny = std::min(features.width, features.height) < std::max(1, opts.quality.tiny_min_side);
    bool need_edges = opts.compute_fingerprint && opts.fingerprint.compute_edge_hash;
    if (opts.compute_quality && !tiny && needs_edge_analysis(features.histogram, opts.quality))
        need_edges = true;
    if (need_edges)
        features.edges = extract_edge_features(rgb, width, height, bands, opts.quality.edges);

    if (opts.compute_quality)
        out.quality = image_quality_from_features(features, classify_junk(features, opts.quality));
    else
        out.quality.ok = true;

    if (opts.compute_fingerprint) {
        out.fingerprint = derive_image_fingerprint(
            rgb, width, height, bands, pixels, features.histogram, features.color, features.edges,
            opts.fingerprint, features.width, features.height);
    }

    out.ok = opts.compute_quality ? out.quality.ok : true;
    return out;
}

ImageAnalysis analyze_image(const std::string& path, const ImageAnalysisOptions& opts)
{
    ImageAnalysis out;
    if (!opts.compute_quality && !opts.compute_fingerprint) {
        out.ok = true;
        out.quality.ok = true;
        return out;
    }

    const AnalysisRgbFrame frame = load_analysis_rgb(path, opts.quality.analyze_max_side);
    if (!frame.ok) {
        out.error = frame.error;
        out.quality.error = frame.error;
        return out;
    }
    return analyze_image_from_rgb(frame.rgb, frame.width, frame.height, frame.bands, opts,
                                  frame.source_width, frame.source_height, frame.file_bytes);
}

} // namespace media
