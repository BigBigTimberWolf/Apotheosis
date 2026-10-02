#pragma once
#include "rule_logic.h"
#include "mem/gpu_image.h"
#include <opencv2/core.hpp>
namespace macros {
bool visionRequested();
void submitVisionFrame(const cv::Mat& image,int64_t captureNs);
void submitVisionFrame(const GpuImage& image,int64_t captureNs);
void configureSources(const std::vector<Program>& programs,bool enabled,int resolution=0);
void stopSources();
void stopSourceEffects();
void appendSources(RuleSnapshot& snapshot);
bool sourceAction(const Action& action,const RuleSnapshot& snapshot,std::string& error);
}
