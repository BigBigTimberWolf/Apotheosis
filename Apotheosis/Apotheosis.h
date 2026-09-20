#ifndef APOTHEOSIS_H
#define APOTHEOSIS_H

#include <atomic>
#include <mutex>

#include "config.h"
#include "i_detector.h"
#include "trt_detector.h"
#include "mouse.h"
#include "detection_buffer.h"
#include "Makcu.h"
#include "MakcuNew.h"

namespace runtime { class InferenceSession; }

extern Config config;
extern TrtDetector trt_detector;
extern IDetector* g_detector;
extern runtime::InferenceSession* g_inference_session;
extern DetectionBuffer detectionBuffer;
extern MakcuConnection* makcuSerial;
extern MakcuNewConnection* makcuNewSerial;
// 第二台 MAKCUNEW(键盘那台)。nullptr = 未配置, 键盘动作回落到 makcuNewSerial。
extern MakcuNewConnection* makcuNewSerialKbd;
class KmboxNetConnection;
extern KmboxNetConnection* kmboxNetSerial;
extern std::atomic<bool> input_method_changed;
extern std::atomic<bool> aiming;

extern std::atomic<bool> session_stop_requested;
extern std::recursive_mutex configMutex;
extern std::mutex inputDeviceMutex;

void createInputDevices();
void assignInputDevices();

#endif // APOTHEOSIS_H
