#pragma once

#include "runtime/gs/gs_types.h"

#include <array>
#include <cstdint>
#include <vector>

struct GSBackendState
{
    static constexpr uint32_t kCachePageSize = 8192u;

    std::array<uint16_t, 512> clut{};
    std::array<uint32_t, 2> clutCbp{};
    uint32_t cachePageBase = UINT32_MAX;
    std::array<uint8_t, kCachePageSize> cacheBytes{};
    GSTransferCommand transfer{};
    GSTransferSnapshot transferState{};
    std::vector<uint8_t> localToHost;
    uint64_t localToHostReadPos = 0u;
};

class GSBackendStateAccess
{
public:
    virtual ~GSBackendStateAccess() = default;

    virtual bool ExportState(GSBackendState &out) = 0;
    virtual bool ImportState(const GSBackendState &state) = 0;
};
