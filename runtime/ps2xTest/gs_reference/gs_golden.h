#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_recording.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GSGolden
{
    struct Recording
    {
        std::vector<GSRecording::Record> records;
        GSRecording::Checkpoint initial;
        size_t submits = 0u;
        size_t checkpoints = 0u;
    };

    bool Load(const std::string &path, Recording &out, std::string &error);

    struct Report
    {
        bool ok = true;
        std::string message;
        size_t checkpointsCompared = 0u;
        size_t firstFailingRecord = SIZE_MAX;
        double seconds = 0.0;
    };

    std::string CompareVram(const uint8_t *expected, const uint8_t *actual, size_t size);
    std::string CompareState(const GSBackendState &expected, const GSBackendState &actual);

    Report Replay(const Recording &recording, GSRasterBackend &backend, bool compare, bool checkHashes = true);

    Report Lockstep(const Recording &recording, GSRasterBackend &reference, GSRasterBackend &candidate);

    std::vector<std::string> ListRecordings(const std::string &directory);
    std::string DefaultDirectory();
}
