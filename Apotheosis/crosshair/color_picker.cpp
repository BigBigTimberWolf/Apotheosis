#include "crosshair/color_picker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace crosshair
{
namespace
{
std::atomic<int> g_armed_token{ 0 };
std::atomic<int> g_pick_half{ kPickHalf };

std::mutex g_result_mutex;
int  g_next_token   = 0;
bool g_result_ready = false;
int  g_result_token = 0;
int  g_result_h = 0;
int  g_result_s = 0;
int  g_result_v = 0;
}

int ArmColorPick(int sampleHalf)
{
    g_pick_half.store(std::clamp(sampleHalf, 0, 8));
    int token;
    {
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_result_ready = false;
        token = ++g_next_token;
        if (token == 0) token = ++g_next_token;
    }
    g_armed_token.store(token);
    return token;
}

void CancelColorPick()
{
    g_armed_token.store(0);
}

bool IsColorPickArmed()
{
    return g_armed_token.load() != 0;
}

int ArmedToken()
{
    return g_armed_token.load();
}

int PickHalf()
{
    return g_pick_half.load();
}

void SubmitPickedColor(int h, int s, int v)
{
    const int token = g_armed_token.load();
    if (token == 0)
        return;
    {
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_result_h = h;
        g_result_s = s;
        g_result_v = v;
        g_result_token = token;
        g_result_ready = true;
    }
    g_armed_token.store(0);
}

bool TakePickedColor(int token, int& h, int& s, int& v)
{
    std::lock_guard<std::mutex> lk(g_result_mutex);
    if (!g_result_ready || g_result_token != token)
        return false;
    h = g_result_h;
    s = g_result_s;
    v = g_result_v;
    g_result_ready = false;
    return true;
}

bool SampleRegionHSV(const cv::Mat& bgr, int cx, int cy, int half,
                     int& h, int& s, int& v)
{
    if (bgr.empty() || bgr.type() != CV_8UC3)
        return false;
    if (cx < 0 || cy < 0 || cx >= bgr.cols || cy >= bgr.rows)
        return false;

    half = std::max(0, half);
    const int x0 = std::max(0, cx - half);
    const int y0 = std::max(0, cy - half);
    const int x1 = std::min(bgr.cols - 1, cx + half);
    const int y1 = std::min(bgr.rows - 1, cy + half);
    const cv::Rect roi(x0, y0, x1 - x0 + 1, y1 - y0 + 1);

    cv::Mat hsv;
    cv::cvtColor(bgr(roi), hsv, cv::COLOR_BGR2HSV);

    std::vector<int> sVals;
    std::vector<int> vVals;
    sVals.reserve(roi.area());
    vVals.reserve(roi.area());

    double sumSin = 0.0;
    double sumCos = 0.0;
    for (int y = 0; y < hsv.rows; ++y)
    {
        const cv::Vec3b* row = hsv.ptr<cv::Vec3b>(y);
        for (int x = 0; x < hsv.cols; ++x)
        {
            const double ang = row[x][0] * (CV_PI / 90.0);
            sumCos += std::cos(ang);
            sumSin += std::sin(ang);
            sVals.push_back(row[x][1]);
            vVals.push_back(row[x][2]);
        }
    }
    if (sVals.empty())
        return false;

    double meanAng = std::atan2(sumSin, sumCos);
    if (meanAng < 0.0)
        meanAng += 2.0 * CV_PI;
    h = static_cast<int>(std::lround(meanAng * (90.0 / CV_PI))) % 180;

    auto median = [](std::vector<int>& a) {
        const size_t mid = a.size() / 2;
        std::nth_element(a.begin(), a.begin() + mid, a.end());
        return a[mid];
    };
    s = median(sVals);
    v = median(vVals);
    return true;
}

}
