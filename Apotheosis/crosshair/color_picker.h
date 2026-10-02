#ifndef CROSSHAIR_COLOR_PICKER_H
#define CROSSHAIR_COLOR_PICKER_H

#include "crosshair/color_lab.h"

namespace cv { class Mat; }

namespace crosshair
{

constexpr int kPickHalf = 2;

int  ArmColorPick(int sampleHalf = kPickHalf);
void CancelColorPick();
bool IsColorPickArmed();
int  ArmedToken();
int  PickHalf();

void SubmitPickedColor(int h, int s, int v);

bool TakePickedColor(int token, int& h, int& s, int& v);

bool SampleRegionHSV(const cv::Mat& bgr, int cx, int cy, int half,
                     int& h, int& s, int& v);

void SetColorLabPreview(std::vector<ColorLabBand> bands);
std::vector<ColorLabBand> ColorLabPreviewBands();

}

#endif // CROSSHAIR_COLOR_PICKER_H
