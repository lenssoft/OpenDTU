// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <AsyncTCP.h>
#include <TaskSchedulerDeclarations.h>
#include <cstdint>
#include <mutex>

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
    void applyPowerLimit(uint16_t limitPct);

    static constexpr uint16_t MODBUS_PORT = 502;
    static constexpr uint8_t  UNIT_ID     = 126;   // Victron dbus-fronius convention
    static constexpr uint16_t REG_BASE    = 40000;
    // Models: SunS(2) + 1(67) + 101(52) + 120(28) + 123(26) + end(2) = 177
    static constexpr uint16_t REG_COUNT   = 180;

    // Absolute register address of WMaxLimPct in Model 123
    // Layout: SunS(2) + Model1(67) + Model101(52) + Model120(28) = 149 offset to Model123
    // Model123: +0=ID, +1=Len, +2=Conn, +3=WMaxLimPct
    static constexpr uint16_t REG_WMAXLIMPCT = REG_BASE + 149 + 3;  // 40152
    static constexpr uint16_t REG_WMAXLIMPCT_ENA = REG_BASE + 149 + 4; // 40153

    Task         _loopTask;
    AsyncServer* _server = nullptr;

    std::mutex _mutex;
    uint16_t   _regs[REG_COUNT] = {};

    // Current power limit percentage: 0-10000 (= 0.00%–100.00%, SF=-2)
    uint16_t _powerLimitPct = 10000;
};

extern SunSpecServerClass SunSpecServer;
