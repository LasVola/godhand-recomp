#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_backend_state.h"
#include "runtime/gs/gs_swizzle.h"
#include "runtime/gs/gs_cpu_raster.h"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class GSBandPool;

class GSCpuBackend final : public GSRasterBackend, public GSBackendStateAccess
{
public:
    GSCpuBackend();
    ~GSCpuBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    bool ExportState(GSBackendState &out) override;
    bool ImportState(const GSBackendState &state) override;

private:
    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    bool DrawSpriteFast(const GSPrimitiveBatch &batch);
    bool DrawTriangleFast(const GSPrimitiveBatch &batch);
    bool PrepareCacheForTexturedDraw();
    void FinishTexturedDraw(uint32_t lastTexelByte);
    uint32_t LastBandTexel() const;
    bool BandsAllowed(const GSRaster::PixelSetup &px, int x0, int y0, int x1, int y1) const;
    unsigned PlanBands(int y0, int y1, int columns, bool allowed);
    void RunBands(int y0, int y1, unsigned bands, const std::function<void(int, int, unsigned)> &work);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);

    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    uint64_t m_clutVersion = 0u;
    GSSwizzle::TextureCache m_textureCache;
    GSRaster::PixelSetup m_pixelSetup;
    GSRaster::TextureSampler m_sampler;
    std::vector<GSRaster::AxisSample> m_columnSamples;
    std::vector<GSRaster::AxisSample> m_rowSamples;
    std::vector<GSRaster::PixelColumn> m_pixelColumns;
    std::vector<float> m_edgeColumns;
    std::vector<uint32_t> m_bandLastTexel;
    std::unique_ptr<GSBandPool> m_bandPool;

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
