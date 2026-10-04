#pragma once

#include <SupportDefs.h>

// "RadeonGfx clocks [upload | start | start-memory | memory
// | watch]": the VBIOS DPM levels,
// the SMC DPM table built from them and the current clocks; changes no
// clock without an action (but starts the SMC firmware if it isn't running).
// upload: writes the MC arbiter timings and the DPM table into SMC RAM;
// start: also enables engine clock DPM, start-memory memory clock DPM too;
// memory: memory clock DPM after start; watch: prints the current clocks for 15 s
status_t PolarisClocks(const char *action);
