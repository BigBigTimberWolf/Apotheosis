#pragma once
#include "control/background_motion.h"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <vector>

namespace runtime {
class BackgroundMotionEstimator {
public:
    static constexpr int thumbnailSide = 160;
    static cv::Size thumbnailSize(cv::Size size) {
        const double scale = std::min(1.0, double(thumbnailSide) / std::max(size.width, size.height));
        return {std::max(1, int(size.width * scale)), std::max(1, int(size.height * scale))};
    }
    static void thumbnail(const cv::Mat& frame, cv::Mat& gray) {
        const auto size = thumbnailSize(frame.size());
        gray.create(size, CV_8UC1);
        const int c = frame.channels();
        for (int y = 0; y < size.height; ++y) for (int x = 0; x < size.width; ++x) {
            const int x0 = x * frame.cols / size.width, x1 = (x + 1) * frame.cols / size.width - 1;
            const int y0 = y * frame.rows / size.height, y1 = (y + 1) * frame.rows / size.height - 1;
            auto luma = [&](int px, int py) {
                const auto* p = frame.ptr<unsigned char>(py) + px * c;
                return c == 1 ? int(p[0]) : (29 * p[0] + 150 * p[1] + 77 * p[2] + 128) >> 8;
            };
            gray.at<unsigned char>(y,x) = (luma(x0,y0)+luma(x1,y0)+luma(x0,y1)+luma(x1,y1)+2)/4;
        }
    }
    void reset() {
        previous_.release(); previousMask_.release(); result_ = {}; ++chain_;
    }
    control::BackgroundMotion update(const cv::Mat& gray, cv::Size original,
                                     const std::vector<cv::Rect>& boxes, int64_t timeUs) {
        if (gray.empty() || gray.type() != CV_8UC1 || original.width <= 0 || original.height <= 0 || timeUs <= 0) {
            reset(); return {};
        }
        if (!previous_.empty() && result_.timeUs == timeUs) return result_;
        const double sx = double(original.width) / gray.cols, sy = double(original.height) / gray.rows;
        cv::Mat mask(gray.size(), CV_8UC1, cv::Scalar(255));
        const cv::Rect bounds(0,0,gray.cols,gray.rows);
        for (const auto& b : boxes) {
            cv::Rect r(int(b.x/sx)-5,int(b.y/sy)-5,int(std::ceil(b.width/sx))+10,int(std::ceil(b.height/sy))+10);
            r &= bounds; if (r.area()>0) mask(r).setTo(0);
        }
        // Crosshair and fixed center HUD must not vote for zero camera motion.
        cv::rectangle(mask, cv::Rect(gray.cols/2-9,gray.rows/2-9,19,19)&bounds, cv::Scalar(0), cv::FILLED);
        bool valid = false;
        control::Vec2 delta;
        double sigma = 0;
        if (!previous_.empty() && previous_.size()==gray.size() && original_==original &&
            timeUs > result_.timeUs && timeUs-result_.timeUs<=200000) {
            std::vector<cv::Point2f> a,b,back;
            cv::goodFeaturesToTrack(previous_,a,48,.03,6,previousMask_,3);
            if (a.size()>=12) {
                std::vector<unsigned char> ab,ba;
                std::vector<float> error,backError;
                cv::calcOpticalFlowPyrLK(previous_,gray,a,b,ab,error,{11,11},2,
                    {cv::TermCriteria::COUNT|cv::TermCriteria::EPS,12,.03});
                cv::calcOpticalFlowPyrLK(gray,previous_,b,back,ba,backError,{11,11},2,
                    {cv::TermCriteria::COUNT|cv::TermCriteria::EPS,12,.03});
                std::vector<float> dx,dy;
                std::vector<size_t> accepted;
                for (size_t i=0;i<a.size();++i) {
                    if (!ab[i] || !ba[i] || error[i]>18 || cv::norm(back[i]-a[i])>.65 ||
                        b[i].x<3 || b[i].y<3 || b[i].x>=gray.cols-3 || b[i].y>=gray.rows-3 ||
                        !mask.at<unsigned char>(int(b[i].y),int(b[i].x))) continue;
                    dx.push_back(b[i].x-a[i].x); dy.push_back(b[i].y-a[i].y); accepted.push_back(i);
                }
                if (accepted.size()>=12) {
                    const double mx=median(dx),my=median(dy);
                    int count=0,cells=0; bool grid[9]{};
                    double residual=0,sumX=0,sumY=0;
                    float minX=float(gray.cols),minY=float(gray.rows),maxX=0,maxY=0;
                    for (size_t i:accepted) {
                        const double ex=b[i].x-a[i].x-mx,ey=b[i].y-a[i].y-my;
                        if (ex*ex+ey*ey>.7*.7) continue;
                        ++count; sumX+=b[i].x-a[i].x; sumY+=b[i].y-a[i].y; residual+=ex*ex+ey*ey;
                        minX=std::min(minX,a[i].x); maxX=std::max(maxX,a[i].x);
                        minY=std::min(minY,a[i].y); maxY=std::max(maxY,a[i].y);
                        grid[std::clamp(int(a[i].y*3/gray.rows),0,2)*3+std::clamp(int(a[i].x*3/gray.cols),0,2)]=true;
                    }
                    for (bool cell:grid) cells+=cell;
                    valid=count>=12 && count>=int(accepted.size()*.7) && cells>=4 &&
                        maxX-minX>gray.cols*.35 && maxY-minY>gray.rows*.35;
                    if (valid) {
                        delta={sumX/count*sx,sumY/count*sy};
                        sigma=std::max(.15,std::sqrt(residual/count))*std::max(sx,sy);
                    }
                }
            }
        }
        if (!valid) { ++chain_; result_.cumulative={}; result_.variance=0; }
        else { result_.cumulative+=delta; result_.variance+=sigma*sigma; }
        result_.chain=chain_; result_.timeUs=timeUs; result_.valid=valid;
        gray.copyTo(previous_); previousMask_=std::move(mask); original_=original;
        return result_;
    }
private:
    static float median(std::vector<float> v) {
        auto middle=v.begin()+v.size()/2; std::nth_element(v.begin(),middle,v.end()); return *middle;
    }
    cv::Mat previous_,previousMask_;
    cv::Size original_;
    uint64_t chain_=0;
    control::BackgroundMotion result_;
};
}
