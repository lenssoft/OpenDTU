// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <AsyncTCP.h>
#include <TaskSchedulerDeclarations.h>
#include <cstdint>
#include <mutex>

// ─── Compile-time defaults ───────────────────────────────────────────────────
// Override in platformio_override.ini with -DSUNSPEC_MANUFACTURER=\"...\" etc.

#ifndef SUNSPEC_MANUFACTURER
#define SUNSPEC_MANUFACTURER "Hoymiles"
#endif
#ifndef SUNSPEC_MODEL
#define SUNSPEC_MODEL "HMS-1600"
#endif
#ifndef SUNSPEC_SERIAL
#define SUNSPEC_SERIAL "HM-SIM-001"
#endif
#ifndef SUNSPEC_MAX_POWER_W
#define SUNSPEC_MAX_POWER_W 1600.0f
#endif

class SunSpecServerClass {
public:
    SunSpecServerClass();
    void init(Scheduler& scheduler);

    // Called by the TCP onData callback (runs on network task)
    void _onNewClient(AsyncClient* client);
    void _onData(AsyncClient* client, const uint8_t* data, size_t len);

private:
    void loop();
    void buildRegisters();

    static constexpr uint16_t MODBUS_PORT = 502;
    static constexpr uint8_t  UNIT_ID     = 126;   // Victron dbus-fronius convention
    static constexpr uint16_t REG_BASE    = 40000;
    // Models 1(67) + 101(52) + 120(28) + 123(26) + SunS(2) + end(2) = 177
    static constexpr uint16_t REG_COUNT   = 180;

    Task         _loopTask;
    AsyncServer* _server = nullptr;

    std::mutex _mutex;
    uint16_t   _regs[REG_COUNT] = {};
};

extern SunSpecServerClass SunSpecServer;