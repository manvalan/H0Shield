#pragma once
#include <Arduino.h>
#include <map>

struct SensorLive {
    bool     occupied  = false;
    uint16_t raw       = 0;
    uint16_t threshold = 512;
    String   rocrailId;
};

struct ToFLive {
    bool     present    = false;
    bool     enabled    = false;
    uint8_t  distanceMm = 0;
    uint8_t  thresholdMm= 35;
    bool     occupied   = false;
    bool     valid      = false;
    String   status;
};

struct SignalLamps {
    bool r = false;
    bool g = false;
    bool v = false;
};

struct SignalLive {
    uint8_t     type   = 0;   // 0 = MAIN, 1 = SHUNT
    String      aspect;
    SignalLamps lamps;
};

struct TurnoutLive {
    String position;   // "straight" | "turnout"
    bool   busy       = false;
};

struct LiveStatus {
    bool   mqttConnected = false;
    int8_t rssi          = 0;

    std::map<uint8_t, SensorLive> sensors;
    ToFLive                       tof;
    std::map<String, SignalLive>  signals;
    std::map<String, TurnoutLive> turnouts;
};
