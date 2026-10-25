/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include "DvMappingTables.h"
#include "DvRpuTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace DVTUNNEL
{

/*!
 * \brief Row-major matrices for blending the GUI into the carrier.
 *
 * toSource maps GUI linear BT.709 to source linear RGB, toCodes maps source PQ
 * RGB to normalized YCbCr, and offset holds the code offsets.
 */
struct DvGuiTransform
{
  float toSource[3][3] = {};
  float toCodes[3][3] = {};
  float offset[3] = {};
};

/*!
 * \brief Builds the GUI transform from the transport colour parameters.
 *
 * \param colour the stream's colour parameters
 * \param m709 the row-major linear BT.709 to BT.2020 matrix
 * \return the transform, or empty if a matrix is not invertible or the colour
 *         space is not supported
 */
std::optional<DvGuiTransform> BuildGuiTransform(const DvColourParams& colour,
                                                const double (&m709)[3][3]);

/*!
 * \brief A slice of a decoded 4:2:0 P010 texture array on the carrier's device.
 */
struct DvPicture
{
  ID3D11Texture2D* texture = nullptr;
  unsigned slice = 0;
  unsigned width = 0;
  unsigned height = 0;
};

constexpr unsigned SCALE_PHASES = 128;

/*!
 * \brief Upscaling weights of the six source samples around a position.
 *
 * The taps run from two samples before the position to three after it. Row k
 * holds the phase u = 1 - fractional position = k / SCALE_PHASES, for
 * k = 0..SCALE_PHASES. Each row sums to 1.
 */
using DvScaleKernel = std::vector<std::array<float, 6>>;

/*!
 * \brief The Catmull-Rom kernel, used by default.
 */
DvScaleKernel CatmullRomKernel();
DvScaleKernel LinearKernel();
DvScaleKernel NearestKernel();

/*!
 * \brief Converts a Kodi CConvolutionKernel table.
 *
 * \param table the kernel's 2 * SCALE_PHASES entries of four weights. Four-tap
 *        kernels hold one phase per entry; six-tap kernels split each phase over
 *        two entries half a table apart.
 * \param sixTaps whether the kernel has six taps
 */
DvScaleKernel ScaleKernelFromKodi(const float* table, bool sixTaps);

/*!
 * \brief Where a picture goes in the carrier frame, and the metadata it carries.
 *
 * mask is in target pixels; outside it the frame is black. A non-empty scaled
 * rectangle (even coordinates) composes the picture at its own size and
 * resamples it into that rectangle.
 */
struct DvCarrierParams
{
  DvRect mask;
  const std::vector<DvPacket>* packets = nullptr;
  DvRect scaled;
};

/*!
 * \brief Produces carrier frames on a D3D11 device.
 *
 * GPU work is recorded on a deferred context and executed with state
 * restoration, so the caller's immediate-context pipeline state is unchanged.
 */
class CDvGpuCarrier
{
public:
  CDvGpuCarrier() = default;
  ~CDvGpuCarrier() = default;
  CDvGpuCarrier(const CDvGpuCarrier&) = delete;
  CDvGpuCarrier& operator=(const CDvGpuCarrier&) = delete;

  /*!
   * \brief Creates the shaders and resources on device.
   *
   * A failure to create the composer leaves reshape mode available; see
   * ComposerStatus().
   *
   * \param device the device the carrier works on
   * \param hr receives the failing result
   * \param message receives a description of the failure
   * \return false if the packing shaders cannot be created
   */
  bool Initialize(ID3D11Device* device, HRESULT& hr, std::string& message);
  void Release();

  bool ComposerAvailable() const { return m_composerAvailable; }
  /*!
   * \brief Sets the kernel used when upscaling; downscaling always uses a
   * widened Catmull-Rom.
   *
   * \return false when kernel does not have SCALE_PHASES + 1 rows or cannot
   *         be uploaded
   */
  bool SetScaleKernel(const DvScaleKernel& kernel);

  /*!
   * \brief Why the composer is unavailable; empty when it is available.
   */
  const std::string& ComposerStatus() const { return m_composerStatus; }

  /*!
   * \brief Packs a single-layer picture reshaped through a lookup table.
   *
   * \param bl the base-layer picture
   * \param padLeft left offset of the unscaled picture on the canvas (even)
   * \param padTop top offset of the unscaled picture on the canvas (even)
   * \param lut the 12-bit output per 10-bit code of each component
   * \param params placement and metadata packets
   * \param target the carrier frame
   */
  bool PackReshape(const DvPicture& bl,
                   unsigned padLeft,
                   unsigned padTop,
                   const DvReshapeLut& lut,
                   const DvCarrierParams& params,
                   ID3D11Texture2D* target);

  /*!
   * \brief Packs a picture composed from prediction tables and an optional
   * enhancement layer.
   *
   * \param bl the base-layer picture, the size of target unless params scales it
   * \param el the half-resolution enhancement layer, or null to compose
   *        without it
   * \param tables prediction and residual tables
   * \param params placement and metadata packets
   * \param target the carrier frame
   */
  bool PackCompose(const DvPicture& bl,
                   const DvPicture* el,
                   const DvTables& tables,
                   const DvCarrierParams& params,
                   ID3D11Texture2D* target);

  /*!
   * \brief Blends the GUI over a packed frame into target.
   *
   * \param frame the packed carrier frame
   * \param packetCount number of metadata packets in frame, which are kept
   * \param gui Kodi's GUI, premultiplied sRGB
   * \param target receives the blended frame; the size of frame
   * \param transform the stream's GUI transform
   */
  bool CompositeGui(ID3D11Texture2D* frame,
                    std::size_t packetCount,
                    ID3D11Texture2D* gui,
                    ID3D11Texture2D* target,
                    const DvGuiTransform& transform);

private:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  struct Texture
  {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    unsigned width = 0;
    unsigned height = 0;
  };

  bool Compose(const DvPicture& bl, const DvPicture* el, const DvTables& tables);
  bool CreatePackingShaders(HRESULT& hr, std::string& message);
  void CreateComposerShaders();
  bool EnsureTexture(Texture& t, unsigned width, unsigned height, DXGI_FORMAT format, bool uav);
  bool EnsureCanvas(unsigned width, unsigned height);
  // The picture's samples become 10-bit codes << shift.
  bool RecordUnpack(const DvPicture& picture, Texture* textures, unsigned shift);
  bool UploadTables(const DvTables* tables, const DvReshapeLut* lut);
  template<typename T>
  bool UpdateConstants(ID3D11Buffer* buffer, const T& data);

  bool RecordCompose(const DvTables& tables, bool useEl);
  // width x height is the target size.
  bool RecordPack(const DvCarrierParams& params,
                  ID3D11Texture2D* target,
                  unsigned width,
                  unsigned height);
  bool Submit();
  void Dispatch(unsigned width, unsigned height, unsigned group);
  void UnbindCompute();

  void Abandon();
  void DrawFullscreen(ID3D11PixelShader* ps,
                      ID3D11RenderTargetView* rtv,
                      unsigned width,
                      unsigned height);

  ComPtr<ID3D11Device> m_device;
  ComPtr<ID3D11DeviceContext> m_immediate;
  ComPtr<ID3D11DeviceContext> m_deferred;

  ComPtr<ID3D11ComputeShader> m_unpackCs;
  ComPtr<ID3D11ComputeShader> m_scaleCs;
  ComPtr<ID3D11ComputeShader> m_reshapeCs;
  ComPtr<ID3D11ComputeShader> m_packCs;
  ComPtr<ID3D11VertexShader> m_fullscreenVs;
  ComPtr<ID3D11PixelShader> m_copyPs;
  ComPtr<ID3D11PixelShader> m_guiPs;
  ComPtr<ID3D11ComputeShader> m_elVerticalCs;
  ComPtr<ID3D11ComputeShader> m_composeLumaCs;
  ComPtr<ID3D11ComputeShader> m_composeChromaCs;
  ComPtr<ID3D11RasterizerState> m_rasterizer;

  ComPtr<ID3D11Buffer> m_tables;
  ComPtr<ID3D11ShaderResourceView> m_tablesSrv;
  ComPtr<ID3D11ShaderResourceView> m_scaleKernel;
  ComPtr<ID3D11Buffer> m_elParams[3];
  ComPtr<ID3D11Buffer> m_lumaParams;
  ComPtr<ID3D11Buffer> m_chromaParams;
  ComPtr<ID3D11Buffer> m_unpackParams;
  ComPtr<ID3D11Buffer> m_scaleParams;
  ComPtr<ID3D11Buffer> m_reshapeParams;
  ComPtr<ID3D11Buffer> m_packParams;
  ComPtr<ID3D11Buffer> m_guiParams;

  Texture m_src[3];
  Texture m_el[3];
  Texture m_elVertical[3];
  Texture m_out[3];
  Texture m_scaled[3];
  Texture m_packed;
  unsigned m_canvasWidth = 0;
  unsigned m_canvasHeight = 0;

  bool m_composerAvailable = false;
  std::string m_composerStatus;
};

} // namespace DVTUNNEL
