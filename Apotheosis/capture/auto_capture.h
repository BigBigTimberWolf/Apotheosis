#ifndef CAPTURE_AUTO_CAPTURE_H
#define CAPTURE_AUTO_CAPTURE_H

#include <atomic>

namespace AutoCapture
{

extern std::atomic<int>  g_saved_total;
extern std::atomic<int>  g_saved_session;
extern std::atomic<bool> g_force_held;
extern std::atomic<bool> g_running;

void auto_capture_thread();

void reset_session_counter();

}

#endif // CAPTURE_AUTO_CAPTURE_H
