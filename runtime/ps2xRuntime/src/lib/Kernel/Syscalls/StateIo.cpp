#include "Common.h"
#include "ps2x/state_io.h"

template <typename Ar>
void serialize(Ar &ar, SifModuleRecord &value)
{
    ps2x::state::fields(ar, value.id, value.path, value.pathKey, value.refCount, value.loaded);
}

namespace
{
    template <typename Ar>
    void serializeSyscallHostState(Ar &ar)
    {
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            ps2x::state::fields(ar, g_rpc_servers, g_rpc_clients, g_rpc_initialized, g_rpc_next_id, g_rpc_packet_index, g_rpc_server_index,
                                g_rpc_active_queue);
        }
        {
            std::lock_guard<std::mutex> lock(g_bootmode_mutex);
            ps2x::state::fields(ar, g_bootmode_initialized, g_bootmode_pool_offset, g_bootmode_addresses);
        }
        {
            std::lock_guard<std::mutex> lock(g_tls_mutex);
            ps2x::state::field(ar, g_tls_index);
        }
        {
            std::lock_guard<std::mutex> lock(g_osd_mutex);
            ps2x::state::fields(ar, g_osd_config_initialized, g_osd_config_raw, g_osd_config2_raw);
        }
        {
            std::lock_guard<std::mutex> lock(g_sif_module_mutex);
            ps2x::state::fields(ar, g_sif_modules_by_id, g_sif_module_id_by_path, g_next_sif_module_id);
        }
    }
}

void saveSyscallHostState(ps2x::state::Writer &out)
{
    serializeSyscallHostState(out);
}

namespace
{
    void relocateLegacyReservedMemory(uint8_t *rdram)
    {
        using namespace ps2x::kernel_memory;
        if (!rdram)
            return;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            for (auto &[sid, server] : g_rpc_servers)
            {
                if (!inLegacyPool(server.sd_ptr, kLegacyRpcServerPoolBase))
                    continue;
                const uint32_t oldPtr = server.sd_ptr;
                const uint32_t newPtr = rpcAllocServerAddr(rdram);
                if (auto *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, newPtr)))
                    sd->sid = static_cast<int>(sid);
                server.sd_ptr = newPtr;
                for (const auto &[clientPtr, clientState] : g_rpc_clients)
                {
                    auto *client = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, clientPtr));
                    if (client && clientState.sid == sid && client->server == oldPtr)
                        client->server = newPtr;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_bootmode_mutex);
            for (const auto &[id, address] : g_bootmode_addresses)
            {
                if (inLegacyPool(address, kLegacyBootModePoolBase))
                {
                    g_bootmode_initialized = false;
                    g_bootmode_pool_offset = 0;
                    g_bootmode_addresses.clear();
                    break;
                }
            }
        }
    }
}

void loadSyscallHostState(ps2x::state::Reader &in, uint8_t *rdram)
{
    serializeSyscallHostState(in);
    relocateLegacyReservedMemory(rdram);
}
