// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SunSpec Modbus TCP server — exposes Hoymiles inverter data as a
 * SunSpec-compliant PV inverter so Victron Venus OS can auto-discover it.
 *
 * Implements SunSpec models:
 *   1   – Common block (device identity)
 *   101 – Single-phase AC inverter measurements
 *   120 – Nameplate ratings
 *   123 – Immediate controls (power limiting)
 *
 * Data is read directly from the OpenDTU Hoymiles inverter objects and the
 * Datastore aggregator — no MQTT or SQLite required.
 *
 * Modbus TCP port 502, unit ID 126 (Victron dbus-fronius convention).
 */

#include "SunSpecServer.h"
#include "Datastore.h"
#include "MessageOutput.h"
#include "NetworkSettings.h"
#include <Hoymiles.h>
#include <cmath>
#include <cstring>
#include <esp_log.h>

#undef TAG
static const char* TAG = "sunspec";

SunSpecServerClass SunSpecServer;

// ─── helpers ─────────────────────────────────────────────────────────────────

static int16_t encodeSf(float value, int sf)
{
    float scaled = value * powf(10.0f, static_cast<float>(-sf));
    scaled = fmaxf(-32768.0f, fminf(32767.0f, scaled));
    return static_cast<int16_t>(roundf(scaled));
}

// Pack a C-string into SunSpec register format: 2 ASCII chars per register,
// big-endian, zero-padded.
static void encodeStr(uint16_t* out, const char* s, size_t numRegs)
{
    size_t len = strlen(s);
    for (size_t i = 0; i < numRegs; i++) {
        uint8_t hi = (2 * i     < len) ? static_cast<uint8_t>(s[2 * i])     : 0u;
        uint8_t lo = (2 * i + 1 < len) ? static_cast<uint8_t>(s[2 * i + 1]) : 0u;
        out[i] = static_cast<uint16_t>((hi << 8) | lo);
    }
}

// ─── SunSpecServerClass ───────────────────────────────────────────────────────

SunSpecServerClass::SunSpecServerClass()
    : _loopTask(5 * TASK_SECOND, TASK_FOREVER, std::bind(&SunSpecServerClass::loop, this))
{
}

void SunSpecServerClass::init(Scheduler& scheduler)
{
    buildRegisters();

    // Start the TCP server once we have an IP address.
    NetworkSettings.onEvent(
        [this](network_event /* event */) {
            if (_server != nullptr) {
                return;
            }
            _server = new AsyncServer(MODBUS_PORT);
            _server->onClient(
                [](void* arg, AsyncClient* client) {
                    static_cast<SunSpecServerClass*>(arg)->_onNewClient(client);
                },
                this);
            _server->begin();
            ESP_LOGI(TAG, "Modbus TCP server listening on port %u (unit ID %u)",
                MODBUS_PORT, UNIT_ID);
        },
        network_event::NETWORK_GOT_IP);

    scheduler.addTask(_loopTask);
    _loopTask.enable();

    ESP_LOGI(TAG, "SunSpec module initialised — %s %s [%s] %.0f W",
        SUNSPEC_MANUFACTURER, SUNSPEC_MODEL, SUNSPEC_SERIAL, SUNSPEC_MAX_POWER_W);
}

void SunSpecServerClass::loop()
{
    buildRegisters();
}

// ─── Register builder ─────────────────────────────────────────────────────────

void SunSpecServerClass::buildRegisters()
{
    // ── Live data from OpenDTU ──────────────────────────────────────────────
    float acPower  = Datastore.getTotalAcPowerEnabled();      // W
    float acEnergy = Datastore.getTotalAcYieldTotalEnabled(); // kWh

    float acVoltage = 230.0f;   // V   — fallback values
    float acFreq    =  50.0f;   // Hz
    float dcVoltage = 380.0f;   // V

    for (uint8_t i = 0; i < Hoymiles.getNumInverters(); i++) {
        auto inv = Hoymiles.getInverterByPos(i);
        if (inv == nullptr || !inv->isReachable()) {
            continue;
        }
        auto* st = inv->Statistics();
        if (st->hasChannelFieldValue(TYPE_AC, CH0, FLD_UAC)) {
            acVoltage = st->getChannelFieldValue(TYPE_AC, CH0, FLD_UAC);
        }
        if (st->hasChannelFieldValue(TYPE_AC, CH0, FLD_F)) {
            acFreq = st->getChannelFieldValue(TYPE_AC, CH0, FLD_F);
        }
        if (st->hasChannelFieldValue(TYPE_DC, CH1, FLD_UDC)) {
            dcVoltage = st->getChannelFieldValue(TYPE_DC, CH1, FLD_UDC);
        }
        break; // use first reachable inverter for grid parameters
    }

    // ── Derived values ──────────────────────────────────────────────────────
    float    acCurrent = (acVoltage > 0.0f) ? acPower / acVoltage  : 0.0f;
    float    dcPower   = acPower * 1.03f;   // ~3% loss estimate
    float    dcCurrent = (dcVoltage > 0.0f) ? dcPower  / dcVoltage : 0.0f;
    uint16_t opState   = (acPower > 0.0f) ? 4u : 2u; // 4=MPPT, 2=Sleeping
    uint32_t energyWh  = static_cast<uint32_t>(acEnergy * 1000.0f);

    // ── Scale factors (match the Python simulator) ──────────────────────────
    static constexpr int SF_POWER   =  0;  // W  → integer watts
    static constexpr int SF_VOLTAGE = -1;  // V  → tenths   (2302 = 230.2 V)
    static constexpr int SF_CURRENT = -2;  // A  → hundredths (198 = 1.98 A)
    static constexpr int SF_ENERGY  =  0;  // Wh → integer
    static constexpr int SF_FREQ    = -2;  // Hz → hundredths (5000 = 50.00 Hz)

    // ── Build register image in a local buffer ──────────────────────────────
    uint16_t tmp[REG_COUNT];
    memset(tmp, 0, sizeof(tmp));

    uint16_t cur = 0; // cursor relative to REG_BASE (i.e., offset from 40000)

    auto w = [&](uint16_t off, uint16_t v) {
        if (off < REG_COUNT) { tmp[off] = v; }
    };
    auto w32 = [&](uint16_t off, uint32_t v) {
        if (off + 1 < REG_COUNT) {
            tmp[off]     = static_cast<uint16_t>(v >> 16);
            tmp[off + 1] = static_cast<uint16_t>(v & 0xFFFF);
        }
    };
    auto wstr = [&](uint16_t off, const char* s, size_t n) {
        if (off + n <= REG_COUNT) { encodeStr(tmp + off, s, n); }
    };

    // ── SunSpec magic "SunS" ────────────────────────────────────────────────
    w32(cur, 0x53756E53u);
    cur += 2;

    // ── Model 1: Common Block ───────────────────────────────────────────────
    w(cur + 0,  1);     // Model ID
    w(cur + 1, 65);     // Length (fixed by spec)
    wstr(cur +  2, SUNSPEC_MANUFACTURER, 16);
    wstr(cur + 18, SUNSPEC_MODEL,        16);
    wstr(cur + 34, "1.0.0",               8);   // version
    wstr(cur + 42, "1.0.0",               8);   // SW version
    wstr(cur + 50, SUNSPEC_SERIAL,       16);
    w(cur + 66, UNIT_ID);
    cur += 67;  // 2 (header) + 65 (data)

    // ── Model 101: Single-Phase Inverter ────────────────────────────────────
    w(cur + 0, 101);    // Model ID
    w(cur + 1,  50);    // Length

    // Current (5 registers: A, AphA, AphB, AphC, A_SF)
    w(cur + 2, static_cast<uint16_t>(encodeSf(acCurrent, SF_CURRENT))); // A
    w(cur + 3, static_cast<uint16_t>(encodeSf(acCurrent, SF_CURRENT))); // AphA
    w(cur + 4, 0xFFFF);                                                  // AphB N/A
    w(cur + 5, 0xFFFF);                                                  // AphC N/A
    w(cur + 6, static_cast<uint16_t>(SF_CURRENT & 0xFFFF));             // A_SF

    // Voltage (7 registers: PPVphAB/BC/CA, PhVphA/B/C, V_SF)
    w(cur + 7,  0xFFFF);                                                  // PPVphAB N/A
    w(cur + 8,  0xFFFF);                                                  // PPVphBC N/A
    w(cur + 9,  0xFFFF);                                                  // PPVphCA N/A
    w(cur + 10, static_cast<uint16_t>(encodeSf(acVoltage, SF_VOLTAGE))); // PhVphA
    w(cur + 11, 0xFFFF);                                                  // PhVphB N/A
    w(cur + 12, 0xFFFF);                                                  // PhVphC N/A
    w(cur + 13, static_cast<uint16_t>(SF_VOLTAGE & 0xFFFF));             // V_SF

    // Power
    w(cur + 14, static_cast<uint16_t>(encodeSf(acPower, SF_POWER)));     // W
    w(cur + 15, static_cast<uint16_t>(SF_POWER & 0xFFFF));               // W_SF

    // Frequency
    w(cur + 16, static_cast<uint16_t>(encodeSf(acFreq, SF_FREQ)));       // Hz
    w(cur + 17, static_cast<uint16_t>(SF_FREQ & 0xFFFF));                // Hz_SF

    // VA / VAr / PF — not available
    w(cur + 18, 0xFFFF); w(cur + 19, 0xFFFF);  // VA, VA_SF
    w(cur + 20, 0xFFFF); w(cur + 21, 0xFFFF);  // VAr, VAr_SF
    w(cur + 22, 0xFFFF); w(cur + 23, 0xFFFF);  // PF, PF_SF

    // Energy (acc32 Wh)
    w32(cur + 24, energyWh);
    w(cur + 26, static_cast<uint16_t>(SF_ENERGY & 0xFFFF));              // WH_SF

    // DC input
    w(cur + 27, static_cast<uint16_t>(encodeSf(dcCurrent, SF_CURRENT))); // DCA
    w(cur + 28, static_cast<uint16_t>(SF_CURRENT & 0xFFFF));             // DCA_SF
    w(cur + 29, static_cast<uint16_t>(encodeSf(dcVoltage, SF_VOLTAGE))); // DCV
    w(cur + 30, static_cast<uint16_t>(SF_VOLTAGE & 0xFFFF));             // DCV_SF
    w(cur + 31, static_cast<uint16_t>(encodeSf(dcPower, SF_POWER)));     // DCW
    w(cur + 32, static_cast<uint16_t>(SF_POWER & 0xFFFF));               // DCW_SF

    // Temperature — not available
    for (uint16_t i = 33; i <= 37; i++) { w(cur + i, 0xFFFF); }

    // Operating state
    w(cur + 38, opState);
    w(cur + 39, 0);       // StVnd

    // Events (32-bit pairs, all zero — already zero from memset)
    // +40..+51 → 6 × uint32 = 12 registers (already 0)

    cur += 52;  // 2 (header) + 50 (data)

    // ── Model 120: Nameplate Ratings ────────────────────────────────────────
    w(cur + 0, 120);    // Model ID
    w(cur + 1,  26);    // Length
    w(cur + 2,   4);    // DERTyp = 4 (PV)
    w(cur + 3, static_cast<uint16_t>(encodeSf(SUNSPEC_MAX_POWER_W, SF_POWER)));  // WRtg
    w(cur + 4, static_cast<uint16_t>(SF_POWER & 0xFFFF));                         // WRtg_SF
    w(cur + 5, static_cast<uint16_t>(encodeSf(SUNSPEC_MAX_POWER_W, SF_POWER)));  // VARtg
    w(cur + 6, static_cast<uint16_t>(SF_POWER & 0xFFFF));                         // VARtg_SF
    for (uint16_t i = 7; i <= 14; i++) { w(cur + i, 0xFFFF); }                   // N/A fields
    w(cur + 15, static_cast<uint16_t>(encodeSf(SUNSPEC_MAX_POWER_W, SF_POWER))); // WMaxRtg
    w(cur + 16, static_cast<uint16_t>(SF_POWER & 0xFFFF));                        // WMaxRtg_SF
    // +17..+27 → remaining fields (0)
    cur += 28;  // 2 (header) + 26 (data)

    // ── Model 123: Immediate Controls ───────────────────────────────────────
    w(cur + 0, 123);    // Model ID
    w(cur + 1,  24);    // Length
    w(cur + 2,   1);    // Conn = connected
    w(cur + 3, 10000);  // WMaxLimPct = 100.00% (100 * 100)
    w(cur + 4,   1);    // WMaxLimPct_Ena = enabled
    // +5..+25 → 0 (already zero)
    cur += 26;  // 2 (header) + 24 (data)

    // ── End block ───────────────────────────────────────────────────────────
    w(cur + 0, 0xFFFF);
    w(cur + 1, 0x0000);

    // ── Swap into live register buffer ──────────────────────────────────────
    std::lock_guard<std::mutex> lock(_mutex);
    memcpy(_regs, tmp, sizeof(tmp));
}

// ─── Modbus TCP connection handling ──────────────────────────────────────────

void SunSpecServerClass::_onNewClient(AsyncClient* client)
{
    ESP_LOGI(TAG, "Modbus connection from %s", client->remoteIP().toString().c_str());

    client->onData(
        [](void* arg, AsyncClient* c, void* data, size_t len) {
            static_cast<SunSpecServerClass*>(arg)->_onData(
                c, static_cast<const uint8_t*>(data), len);
        },
        this);

    client->onDisconnect(
        [](void* /* arg */, AsyncClient* c) {
            ESP_LOGI(TAG, "Modbus client disconnected");
            delete c;
        },
        nullptr);

    client->onError(
        [](void* /* arg */, AsyncClient* /* c */, int8_t err) {
            ESP_LOGW(TAG, "Modbus client error %d", err);
        },
        nullptr);
}

void SunSpecServerClass::_onData(AsyncClient* client, const uint8_t* data, size_t len)
{
    // Minimum valid Modbus TCP read request:
    //   MBAP header (6 B): TransID(2) + ProtoID(2) + Length(2)
    //   PDU        (6 B): UnitID(1) + FC(1) + StartAddr(2) + Quantity(2)
    if (len < 12) {
        return;
    }

    uint16_t txId   = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    uint16_t proto  = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    uint8_t  unitId = data[6];
    uint8_t  fc     = data[7];

    if (proto != 0) {
        return; // Not Modbus protocol
    }

    // Helper to send a Modbus exception response
    auto sendException = [&](uint8_t exceptionCode) {
        uint8_t resp[9] = {
            static_cast<uint8_t>(txId >> 8), static_cast<uint8_t>(txId & 0xFF),
            0, 0,           // Protocol ID
            0, 3,           // Length: UnitID(1) + FC(1) + ExcCode(1)
            unitId,
            static_cast<uint8_t>(fc | 0x80),
            exceptionCode
        };
        client->write(reinterpret_cast<const char*>(resp), sizeof(resp));
    };

    // Only FC 0x03 (Read Holding Registers) is needed for SunSpec
    if (fc != 0x03) {
        sendException(0x01); // Illegal Function
        return;
    }

    uint16_t startAddr = (static_cast<uint16_t>(data[8])  << 8) | data[9];
    uint16_t quantity  = (static_cast<uint16_t>(data[10]) << 8) | data[11];

    if (quantity == 0 || quantity > 125
        || startAddr < REG_BASE
        || startAddr + quantity > REG_BASE + REG_COUNT)
    {
        sendException(0x02); // Illegal Data Address
        return;
    }

    // Response: MBAP(6) + UnitID(1) + FC(1) + ByteCount(1) + Data(quantity*2)
    uint16_t byteCount = quantity * 2;
    uint16_t mbapLen   = 3 + byteCount; // UnitID + FC + ByteCount + Data

    // Stack buffer (max: 9 + 125*2 = 259 bytes)
    uint8_t resp[9 + 125 * 2];
    resp[0] = static_cast<uint8_t>(txId >> 8);
    resp[1] = static_cast<uint8_t>(txId & 0xFF);
    resp[2] = 0; resp[3] = 0;                                  // Protocol ID
    resp[4] = static_cast<uint8_t>(mbapLen >> 8);
    resp[5] = static_cast<uint8_t>(mbapLen & 0xFF);
    resp[6] = unitId;
    resp[7] = 0x03;
    resp[8] = static_cast<uint8_t>(byteCount);

    {
        std::lock_guard<std::mutex> lock(_mutex);
        uint16_t idx = startAddr - REG_BASE;
        for (uint16_t i = 0; i < quantity; i++) {
            resp[9 + i * 2]     = static_cast<uint8_t>(_regs[idx + i] >> 8);
            resp[9 + i * 2 + 1] = static_cast<uint8_t>(_regs[idx + i] & 0xFF);
        }
    }

    client->write(reinterpret_cast<const char*>(resp), 9 + byteCount);
}