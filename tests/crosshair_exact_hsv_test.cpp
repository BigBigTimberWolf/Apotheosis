#include "crosshair/crosshair_detector.h"

#include <opencv2/imgproc.hpp>

int main() {
    cv::Mat frame(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::rectangle(frame, cv::Rect(47, 47, 7, 7), cv::Scalar(0, 255, 0), cv::FILLED);

    crosshair::CrosshairDetectorSettings settings;
    settings.enabled = true;
    settings.rect_w = 100;
    settings.rect_h = 100;
    settings.close_radius = 0;
    settings.colors = {crosshair::CrosshairColorBand{}};
    auto& band = settings.colors.front();
    band.h_low = 55;
    band.h_high = 65;
    band.s_min = 0;
    band.s_max = 100;
    band.v_min = 200;

    crosshair::CrosshairDetector detector;
    band.exact_hsv = true;
    if (detector.detect(frame, settings)) return 1;
    band.s_max = 255;
    if (!detector.detect(frame, settings)) return 2;
    band.s_max = 100;
    band.exact_hsv = false;
    if (!detector.detect(frame, settings)) return 3;
    // Four separated single-pixel arms must contribute to the whole centroid.
    frame.setTo(cv::Scalar(0, 0, 0));
    for (const auto p : {cv::Point(40, 50), cv::Point(60, 50), cv::Point(50, 40), cv::Point(50, 60)})
        frame.at<cv::Vec3b>(p) = cv::Vec3b(0, 255, 0);
    settings.algorithm = 1;
    band.exact_hsv = true;
    band.s_max = 255;
    settings.min_pixel_count = 4;
    settings.close_radius = 7;
    const auto split = detector.detect(frame, settings);
    if (!split || cv::norm(*split - cv::Point2f(50, 50)) > 0.001) return 4;
    settings.min_pixel_count = 5;
    if (detector.detect(frame, settings)) return 5; // Selected cluster must satisfy the threshold.
    settings.min_pixel_count = 4;
    settings.colors.push_back(band); // Overlapping bands count each pixel once.
    const auto duplicate = detector.detect(frame, settings);
    if (!duplicate || cv::norm(*duplicate - cv::Point2f(50, 50)) > 0.001) return 6;
    frame.setTo(cv::Scalar(0, 0, 0));
    if (detector.detect(frame, settings)) return 7;
    frame.at<cv::Vec3b>(20, 10) = cv::Vec3b(0, 255, 0);
    frame.at<cv::Vec3b>(21, 11) = cv::Vec3b(0, 255, 0);
    settings.min_pixel_count = 2;
    const auto integerMean = detector.detect(frame, settings);
    if (!integerMean || *integerMean != cv::Point2f(10,20)) return 8;
    frame.setTo(cv::Scalar(0,0,0));
    frame.at<cv::Vec3b>(17, 12) = cv::Vec3b(0, 0, 255);
    frame.at<cv::Vec3b>(17, 13) = cv::Vec3b(0, 0, 255);
    settings.colors.resize(1);
    settings.colors[0].h_low = 170; settings.colors[0].h_high = 10;
    const auto wrapped = detector.detect(frame, settings);
    if (!wrapped || *wrapped != cv::Point2f(12,17)) return 9;
    settings.colors=crosshair::default_red_bands();
    settings.min_pixel_count=4;
    // A larger background patch must neither move nor reverse the aim error.
    for(int backgroundX : {20,80}) {
        frame.setTo(cv::Scalar(0,0,0));
        cv::rectangle(frame,cv::Rect(49,49,3,3),cv::Scalar(0,0,255),cv::FILLED);
        cv::rectangle(frame,cv::Rect(backgroundX-4,46,9,9),cv::Scalar(0,0,255),cv::FILLED);
        const auto p=detector.detect(frame,settings);
        if(!p || *p!=cv::Point2f(50,50))return 10;
    }
    // Separate compact dots are alternatives, not one combined centroid.
    frame.setTo(cv::Scalar(0,0,0));
    cv::rectangle(frame,cv::Rect(49,49,3,3),cv::Scalar(0,0,255),cv::FILLED);
    cv::rectangle(frame,cv::Rect(68,49,3,3),cv::Scalar(0,0,255),cv::FILLED);
    if(detector.detect(frame,settings)!=std::optional<cv::Point2f>({50,50}))return 11;
    // Noise, full colour flashes, and clipped blobs must be misses.
    frame.setTo(cv::Scalar(0,0,0));
    frame.at<cv::Vec3b>(50,50)=cv::Vec3b(0,0,255);
    settings.min_pixel_count=1;
    if(detector.detect(frame,settings))return 12;
    frame.setTo(cv::Scalar(0,0,255));
    if(detector.detect(frame,settings))return 13;
    frame.setTo(cv::Scalar(0,0,0));
    cv::rectangle(frame,cv::Rect(0,45,5,5),cv::Scalar(0,0,255),cv::FILLED);
    if(detector.detect(frame,settings))return 14;
    // Current-frame movement remains immediate; no smoothing of the coordinate.
    frame.setTo(cv::Scalar(0,0,0));
    cv::rectangle(frame,cv::Rect(49,64,3,3),cv::Scalar(0,0,255),cv::FILLED);
    if(detector.detect(frame,settings)!=std::optional<cv::Point2f>({50,65}))return 15;
    return 0;
}
