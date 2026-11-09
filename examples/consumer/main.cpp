#include "images/analyze.hpp"

#include <iostream>

// Links pm::image from the install prefix. A missing file is a successful link.
int main()
{
    const media::ImageAnalysis analysis = media::analyze_image("pm-image-consumer-missing.jpg");
    if (analysis.ok) {
        std::cerr << "expected a missing file to fail analysis\n";
        return 1;
    }
    std::cout << analysis.error << "\n";
    return 0;
}
