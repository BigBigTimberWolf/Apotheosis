#ifndef APOTHEOSIS_H
#define APOTHEOSIS_H

#include <atomic>
#include <mutex>
#include <memory>
#include <string>

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
extern std::string kmboxNetLastError; // guarded by inputDeviceMutex
extern std::shared_ptr<mouse_driver::IDriver> dhzboxDriver; // guarded by inputDeviceMutex
extern std::shared_ptr<mouse_driver::IDriver> ferrumDriver; // guarded by inputDeviceMutex
namespace mouse_driver { class WindowsDriver; }
namespace mouse_driver { class CpboxDriver; }
extern std::shared_ptr<mouse_driver::WindowsDriver> windowsDriver; // guarded by inputDeviceMutex
extern std::shared_ptr<mouse_driver::IDriver> catDriver; // guarded by inputDeviceMutex
extern std::shared_ptr<mouse_driver::CpboxDriver> cpboxDriver; // guarded by inputDeviceMutex
extern std::string catLastError;
extern std::string ferrumLastError;
extern std::string dhzboxLastError;
extern std::string cpboxLastError;
extern std::atomic<bool> input_method_changed;
extern std::atomic<bool> aiming;

extern std::atomic<bool> session_stop_requested;
extern std::recursive_mutex configMutex;
extern std::mutex inputDeviceMutex;

void createInputDevices();
void assignInputDevices();
void reconnectMouseDevice();
void reconnectKeyboardDevice();

#endif // APOTHEOSIS_H
