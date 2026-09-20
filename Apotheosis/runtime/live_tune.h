#pragma once

namespace live_tune
{
void start(int poll_ms = 200);

void stop();

bool active();

void poll_now();
}
