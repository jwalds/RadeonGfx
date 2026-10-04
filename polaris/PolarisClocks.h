#pragma once

#include <SupportDefs.h>

// "RadeonGfx clocks [action]": the VBIOS DPM levels, the SMC DPM table built
// from them and the current clocks; changes no clock without an action (but
// starts the SMC firmware if it isn't running). Actions:
//   upload        writes the MC arbiter timings and the DPM table into SMC RAM
//   start         upload, then engine clock DPM
//   start-memory  upload, then engine and memory clock DPM
//   memory        memory clock DPM after start
//   watch         prints the current clocks for 15 s
status_t PolarisClocks(const char *action);

class PolarisSmu;

// engine and memory clock DPM as "clocks start" and "clocks memory" do, for
// the server; nothing if DPM already runs
status_t PolarisStartPowerManagement(PolarisSmu &smu);
