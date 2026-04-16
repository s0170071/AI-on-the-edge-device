#pragma once

#include <string>
#include <vector>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "../../include/defines.h"

struct FlowResultSnapshot {
    SemaphoreHandle_t mutex;

    // Cached JSON output
    std::string jsonCache;

    // Per-number readout values
    struct NumberResult {
        std::string name;
        std::string value;
        std::string rawValue;
        std::string preValue;
        std::string error;
        std::string rate;
        std::string timestamp;
    };
    std::vector<NumberResult> numberResults;

    // Status
    std::string aktstatus;
    std::string aktstatusWithTime;

    // Readout cache
    std::string readoutValue;       // getReadout(false, false, 0)
    std::string readoutRawValue;    // getReadout(true, false, 0)
    std::string readoutNoError;     // getReadout(false, true, 0)
    std::string readoutAllValue;    // getReadoutAll(READOUT_TYPE_VALUE)
    std::string readoutAllPrevalue; // getReadoutAll(READOUT_TYPE_PREVALUE)
    std::string readoutAllRaw;      // getReadoutAll(READOUT_TYPE_RAWVALUE)
    std::string readoutAllError;    // getReadoutAll(READOUT_TYPE_ERROR)

    int roundCount;
    bool valid;

    FlowResultSnapshot() : mutex(NULL), roundCount(0), valid(false) {
        mutex = xSemaphoreCreateMutex();
    }
};

extern FlowResultSnapshot resultSnapshot;
