#include "ToFManager.h"
#include "RocrailProtocol.h"

void ToFManager::_publishOccupancy(bool occupied) {
    if (_rocrail) {
        _rocrail->publishToFFeedback(occupied, *_mqtt);
    }
    String payload = occupied ? "occupied" : "free";
    _mqtt->publish("tof/occupancy", payload, true);
}
