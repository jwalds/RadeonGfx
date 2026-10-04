#pragma once

#include <SupportDefs.h>

// "RadeonGfx clocks": the VBIOS DPM levels and the current clocks (starts
// the SMC firmware if it isn't running; changes no clock)
status_t PolarisClocks();
