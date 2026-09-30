#ifndef CROSSHAIR_DETECTOR_H
#define CROSSHAIR_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <optional>
#include <string>
#include <vector>

namespace crosshair
{

struct CrosshairColorBand
{
    std::string name = "Red-Low";
    bool enabled = true;
    int h_low  = 0;
    int h_high = 10;
    int s_min  = 120;
    int s_max  = 255;
    int v_min  = 120;
    int v_max  = 255;
};

struct CrosshairDetectorSettings
{
    bool enabled = false;

    int rect_w = 64;
    int rect_h = 64;
    int offset_y = 0;

    std::vector<CrosshairColorBand> colors;

    int min_pixel_count = 4;

    int close_radius = 1;
};

std::vector<CrosshairColorBand> default_red_bands();

class CrosshairDetector
{
public:
    std::optional<cv::Point2f> detect(const cv::Mat& bgrFrame,
                                      const CrosshairDetectorSettings& settings) const;
};

}

#endif // CROSSHAIR_DETECTOR_H
