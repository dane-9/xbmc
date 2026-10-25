/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DvGpuCarrier.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

// Bytecode compiled from system/shaders at build time.
#include "dovi_compose_chroma.h"
#include "dovi_compose_luma.h"
#include "dovi_copy_ps.h"
#include "dovi_el_vertical.h"
#include "dovi_fullscreen_vs.h"
#include "dovi_gui_ps.h"
#include "dovi_pack.h"
#include "dovi_reshape.h"
#include "dovi_scale.h"
#include "dovi_unpack_p010.h"

namespace DVTUNNEL
{
namespace
{

using Microsoft::WRL::ComPtr;
using Mat3 = std::array<std::array<double, 3>, 3>;

// Shared table buffer layout: Q16 prediction [0, 3072), signed Q16 residual
// [3072, 6144), reshape LUT [6144, 9216); 1024 entries per component. The
// shaders use the same offsets.
constexpr unsigned TABLE_PREDICT = 0;
constexpr unsigned TABLE_RESIDUAL = 3072;
constexpr unsigned TABLE_RESHAPE = 6144;
constexpr unsigned TABLE_ENTRIES = 9216;

HRESULT CreateShader(ID3D11Device* device,
                     const BYTE* code,
                     std::size_t size,
                     ComPtr<ID3D11ComputeShader>& shader)
{
  return device->CreateComputeShader(code, size, nullptr, &shader);
}

HRESULT CreateShader(ID3D11Device* device,
                     const BYTE* code,
                     std::size_t size,
                     ComPtr<ID3D11VertexShader>& shader)
{
  return device->CreateVertexShader(code, size, nullptr, &shader);
}

HRESULT CreateShader(ID3D11Device* device,
                     const BYTE* code,
                     std::size_t size,
                     ComPtr<ID3D11PixelShader>& shader)
{
  return device->CreatePixelShader(code, size, nullptr, &shader);
}

HRESULT CreateConstantBuffer(ID3D11Device* device, UINT size, ComPtr<ID3D11Buffer>& buffer)
{
  const CD3D11_BUFFER_DESC desc((size + 15) & ~15u, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC,
                                D3D11_CPU_ACCESS_WRITE);
  return device->CreateBuffer(&desc, nullptr, &buffer);
}

void PutDouble(double v, std::uint32_t* out)
{
  std::uint64_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  out[0] = static_cast<std::uint32_t>(bits);
  out[1] = static_cast<std::uint32_t>(bits >> 32);
}

std::optional<Mat3> Invert(const Mat3& m)
{
  Mat3 out;
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  if (!std::isfinite(det) || det == 0.0)
    return {};
  for (int r = 0; r < 3; ++r)
  {
    for (int c = 0; c < 3; ++c)
    {
      // Cofactor of (c, r), i.e. the adjugate.
      const int r0 = (c + 1) % 3, r1 = (c + 2) % 3;
      const int c0 = (r + 1) % 3, c1 = (r + 2) % 3;
      out[r][c] = (m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]) / det;
    }
  }
  return out;
}

Mat3 Multiply(const Mat3& a, const Mat3& b)
{
  Mat3 out{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      for (int k = 0; k < 3; ++k)
        out[r][c] += a[r][k] * b[k][c];
  return out;
}

struct ElParams
{
  std::uint32_t inWidth, inHeight, luma, pad;
};

struct LumaParams
{
  std::uint32_t width, height, useEl, elWidth;
};

struct ChromaParams
{
  std::uint32_t chromaWidth, chromaHeight, lumaWidth, useEl;
  std::uint32_t elWidth, mmrMask, orderCb, orderCr;
  std::uint32_t pivotLow[4];
  std::uint32_t pivotHigh[4];
  std::uint32_t scale[4];
  std::uint32_t mmr[22][4];
};

struct UnpackParams
{
  std::uint32_t width, height, chromaWidth, chromaHeight;
  std::uint32_t shift, pad[3];
};

struct ScaleParams
{
  std::uint32_t width, height, srcWidth, srcHeight;
  std::uint32_t dstLeft, dstTop, dstWidth, dstHeight;
};

struct ReshapeParams
{
  std::uint32_t width, height, padLeft, padTop;
  std::uint32_t picWidth, picHeight, pad[2];
};

struct PackParams
{
  std::uint32_t width, height, bits, pad;
  std::uint32_t mask[4];
  std::uint32_t packets[MAX_PACKETS * PACKET_BYTES / 4];
};

struct GuiParams
{
  float toSource[3][4];
  float toCodes[3][4];
  float offset[4];
  std::uint32_t width, bits, guiWidth, guiHeight;
};

} // namespace

std::optional<DvGuiTransform> BuildGuiTransform(const DvColourParams& colour,
                                                const double (&m709)[3][3])
{
  Mat3 ycc;
  for (int i = 0; i < 9; ++i)
    ycc[i / 3][i % 3] = colour.ycc[i] / 8192.0;
  const std::optional<Mat3> codes = Invert(ycc);
  if (!codes)
    return {};

  Mat3 toSource;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      toSource[r][c] = m709[r][c];

  if (colour.colorSpace == 2)
  {
    Mat3 lms;
    for (int i = 0; i < 9; ++i)
      lms[i / 3][i % 3] = colour.lms[i] / 16384.0;
    const std::optional<Mat3> lmsInverse = Invert(lms);
    if (!lmsInverse)
      return {};
    const Mat3 rgbToLms = {{{7222 / 16384.0, 8771 / 16384.0, 390 / 16384.0},
                            {2654 / 16384.0, 12430 / 16384.0, 1300 / 16384.0},
                            {0.0, 422 / 16384.0, 15962 / 16384.0}}};
    toSource = Multiply(Multiply(*lmsInverse, rgbToLms), toSource);
  }
  else if (colour.colorSpace != 0)
    return {};

  DvGuiTransform out;
  for (int r = 0; r < 3; ++r)
  {
    for (int c = 0; c < 3; ++c)
    {
      if (!std::isfinite(toSource[r][c]) || !std::isfinite((*codes)[r][c]))
        return {};
      out.toSource[r][c] = static_cast<float>(toSource[r][c]);
      out.toCodes[r][c] = static_cast<float>((*codes)[r][c]);
    }
    out.offset[r] = static_cast<float>(colour.offset[r] / 268435456.0);
  }
  return out;
}

bool CDvGpuCarrier::Initialize(ID3D11Device* device, HRESULT& hr, std::string& message)
{
  Release();
  hr = S_OK;
  if (!device)
  {
    hr = E_INVALIDARG;
    message = "no device";
    return false;
  }
  if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
  {
    hr = DXGI_ERROR_UNSUPPORTED;
    message = "feature level 11_0 is required for compute shaders";
    return false;
  }

  m_device = device;
  m_device->GetImmediateContext(&m_immediate);
  hr = m_device->CreateDeferredContext(0, &m_deferred);
  if (FAILED(hr))
  {
    message = "cannot create a deferred context";
    Release();
    return false;
  }
  if (!CreatePackingShaders(hr, message))
  {
    Release();
    return false;
  }
  if (!SetScaleKernel(CatmullRomKernel()))
  {
    hr = E_FAIL;
    message = "cannot create the scaling kernel";
    Release();
    return false;
  }
  CreateComposerShaders();
  return true;
}

namespace
{

DvScaleKernel SampledKernel(double (*weight)(double distance))
{
  DvScaleKernel kernel(SCALE_PHASES + 1);
  for (unsigned phase = 0; phase <= SCALE_PHASES; ++phase)
  {
    const double u = static_cast<double>(phase) / SCALE_PHASES;
    for (int k = 0; k < 6; ++k)
      kernel[phase][k] = static_cast<float>(weight(u + k - 3));
  }
  return kernel;
}

} // namespace

DvScaleKernel CatmullRomKernel()
{
  return SampledKernel(
      [](double x)
      {
        x = std::abs(x);
        if (x < 1.0)
          return (1.5 * x - 2.5) * x * x + 1.0;
        if (x < 2.0)
          return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
        return 0.0;
      });
}

DvScaleKernel LinearKernel()
{
  return SampledKernel([](double x) { return std::max(0.0, 1.0 - std::abs(x)); });
}

DvScaleKernel NearestKernel()
{
  // Halfway between two samples takes the later one.
  return SampledKernel([](double x) { return x > -0.5 && x <= 0.5 ? 1.0 : 0.0; });
}

DvScaleKernel ScaleKernelFromKodi(const float* table, bool sixTaps)
{
  DvScaleKernel kernel(SCALE_PHASES + 1);
  for (unsigned phase = 0; phase < SCALE_PHASES; ++phase)
  {
    const float* a = table + (sixTaps ? phase : 2 * phase) * 4;
    const float* b = table + (phase + SCALE_PHASES) * 4;
    kernel[phase] = sixTaps ? std::array<float, 6>{a[0], b[0], a[1], b[1], a[2], b[2]}
                            : std::array<float, 6>{0.0f, a[0], a[1], a[2], a[3], 0.0f};
  }
  // u = 1 is u = 0 one sample earlier.
  const auto& zero = kernel[0];
  kernel[SCALE_PHASES] = {zero[1], zero[2], zero[3], zero[4], zero[5], 0.0f};
  return kernel;
}

bool CDvGpuCarrier::SetScaleKernel(const DvScaleKernel& kernel)
{
  if (!m_device || kernel.size() != SCALE_PHASES + 1)
    return false;
  const CD3D11_BUFFER_DESC desc(static_cast<UINT>(kernel.size() * sizeof(kernel[0])),
                                D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_IMMUTABLE);
  const D3D11_SUBRESOURCE_DATA data{kernel.data(), 0, 0};
  const CD3D11_SHADER_RESOURCE_VIEW_DESC view(D3D11_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R32_FLOAT, 0,
                                              static_cast<UINT>(kernel.size() * 6));
  ComPtr<ID3D11Buffer> buffer;
  ComPtr<ID3D11ShaderResourceView> srv;
  if (FAILED(m_device->CreateBuffer(&desc, &data, &buffer)) ||
      FAILED(m_device->CreateShaderResourceView(buffer.Get(), &view, &srv)))
    return false;
  m_scaleKernel = srv;
  return true;
}

bool CDvGpuCarrier::CreatePackingShaders(HRESULT& hr, std::string& message)
{
  ID3D11Device* d = m_device.Get();
  if (FAILED(hr = CreateShader(d, dovi_unpack_p010, sizeof(dovi_unpack_p010), m_unpackCs)) ||
      FAILED(hr = CreateShader(d, dovi_scale, sizeof(dovi_scale), m_scaleCs)) ||
      FAILED(hr = CreateShader(d, dovi_reshape, sizeof(dovi_reshape), m_reshapeCs)) ||
      FAILED(hr = CreateShader(d, dovi_pack, sizeof(dovi_pack), m_packCs)) ||
      FAILED(hr =
                 CreateShader(d, dovi_fullscreen_vs, sizeof(dovi_fullscreen_vs), m_fullscreenVs)) ||
      FAILED(hr = CreateShader(d, dovi_copy_ps, sizeof(dovi_copy_ps), m_copyPs)) ||
      FAILED(hr = CreateShader(d, dovi_gui_ps, sizeof(dovi_gui_ps), m_guiPs)))
  {
    message = "cannot create the packing shaders";
    return false;
  }

  CD3D11_RASTERIZER_DESC raster(D3D11_DEFAULT);
  raster.CullMode = D3D11_CULL_NONE;
  hr = d->CreateRasterizerState(&raster, &m_rasterizer);
  if (FAILED(hr))
  {
    message = "cannot create rasterizer state";
    return false;
  }

  const CD3D11_BUFFER_DESC tablesDesc(TABLE_ENTRIES * 4, D3D11_BIND_SHADER_RESOURCE,
                                      D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  hr = d->CreateBuffer(&tablesDesc, nullptr, &m_tables);
  if (SUCCEEDED(hr))
  {
    const CD3D11_SHADER_RESOURCE_VIEW_DESC view(D3D11_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R32_SINT, 0,
                                                TABLE_ENTRIES);
    hr = d->CreateShaderResourceView(m_tables.Get(), &view, &m_tablesSrv);
  }
  for (auto& buffer : m_elParams)
    if (SUCCEEDED(hr))
      hr = CreateConstantBuffer(d, sizeof(ElParams), buffer);
  if (SUCCEEDED(hr))
    hr = CreateConstantBuffer(d, sizeof(LumaParams), m_lumaParams);
  if (SUCCEEDED(hr))
    hr = CreateConstantBuffer(d, sizeof(ChromaParams), m_chromaParams);
  if (SUCCEEDED(hr))
    hr = CreateConstantBuffer(d, sizeof(UnpackParams), m_unpackParams);
  if (SUCCEEDED(hr))
    hr = CreateConstantBuffer(d, sizeof(ScaleParams), m_scaleParams);
  if (SUCCEEDED(hr))
    hr = CreateConstantBuffer(d, sizeof(ReshapeParams), m_reshapeParams);
  if (SUCCEEDED(hr))
    hr = CreateConstantBuffer(d, sizeof(PackParams), m_packParams);
  if (SUCCEEDED(hr))
    hr = CreateConstantBuffer(d, sizeof(GuiParams), m_guiParams);
  if (FAILED(hr))
  {
    message = "cannot create buffers";
    return false;
  }
  return true;
}

void CDvGpuCarrier::CreateComposerShaders()
{
  m_composerAvailable = false;
  D3D11_FEATURE_DATA_DOUBLES doubles{};
  if (FAILED(m_device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles))) ||
      !doubles.DoublePrecisionFloatShaderOps)
  {
    m_composerStatus = "(no double-precision shader support)";
    return;
  }

  ID3D11Device* d = m_device.Get();
  if (FAILED(CreateShader(d, dovi_el_vertical, sizeof(dovi_el_vertical), m_elVerticalCs)) ||
      FAILED(CreateShader(d, dovi_compose_luma, sizeof(dovi_compose_luma), m_composeLumaCs)) ||
      FAILED(CreateShader(d, dovi_compose_chroma, sizeof(dovi_compose_chroma), m_composeChromaCs)))
  {
    m_composerStatus = "(cannot create the composer shaders)";
    return;
  }
  m_composerStatus.clear();
  m_composerAvailable = true;
}

void CDvGpuCarrier::Release()
{
  for (Texture* group : {m_src, m_el, m_elVertical, m_out, m_scaled})
    for (int p = 0; p < 3; ++p)
      group[p] = {};
  m_packed = {};
  m_canvasWidth = m_canvasHeight = 0;
  m_tablesSrv.Reset();
  m_scaleKernel.Reset();
  m_tables.Reset();
  for (auto& buffer : m_elParams)
    buffer.Reset();
  m_lumaParams.Reset();
  m_chromaParams.Reset();
  m_unpackParams.Reset();
  m_reshapeParams.Reset();
  m_packParams.Reset();
  m_guiParams.Reset();
  m_rasterizer.Reset();
  m_unpackCs.Reset();
  m_scaleCs.Reset();
  m_scaleParams.Reset();
  m_reshapeCs.Reset();
  m_packCs.Reset();
  m_fullscreenVs.Reset();
  m_copyPs.Reset();
  m_guiPs.Reset();
  m_elVerticalCs.Reset();
  m_composeLumaCs.Reset();
  m_composeChromaCs.Reset();
  m_deferred.Reset();
  m_immediate.Reset();
  m_device.Reset();
  m_composerAvailable = false;
  m_composerStatus.clear();
}

bool CDvGpuCarrier::EnsureTexture(
    Texture& t, unsigned width, unsigned height, DXGI_FORMAT format, bool uav)
{
  if (t.texture && t.width == width && t.height == height && (t.uav != nullptr) == uav)
    return true;
  t = {};
  if (width == 0 || height == 0)
    return false;

  UINT bind = D3D11_BIND_SHADER_RESOURCE;
  if (uav)
    bind |= D3D11_BIND_UNORDERED_ACCESS;
  const CD3D11_TEXTURE2D_DESC desc(format, width, height, 1, 1, bind);
  if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &t.texture)) ||
      FAILED(m_device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv)) ||
      (uav && FAILED(m_device->CreateUnorderedAccessView(t.texture.Get(), nullptr, &t.uav))))
  {
    t = {};
    return false;
  }
  t.width = width;
  t.height = height;
  return true;
}

bool CDvGpuCarrier::EnsureCanvas(unsigned width, unsigned height)
{
  if (width < 2 || height < 2 || (width & 1) || (height & 1))
    return false;
  m_canvasWidth = width;
  m_canvasHeight = height;
  return EnsureTexture(m_out[0], width, height, DXGI_FORMAT_R16_UINT, true) &&
         EnsureTexture(m_out[1], width / 2, height / 2, DXGI_FORMAT_R16_UINT, true) &&
         EnsureTexture(m_out[2], width / 2, height / 2, DXGI_FORMAT_R16_UINT, true);
}

bool CDvGpuCarrier::RecordUnpack(const DvPicture& picture, Texture* textures, unsigned shift)
{
  const unsigned width = picture.width;
  const unsigned height = picture.height;
  const unsigned chromaWidth = (width + 1) / 2;
  const unsigned chromaHeight = (height + 1) / 2;
  const CD3D11_SHADER_RESOURCE_VIEW_DESC luma(D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
                                              DXGI_FORMAT_R16_UINT, 0, 1, picture.slice, 1);
  const CD3D11_SHADER_RESOURCE_VIEW_DESC chroma(D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
                                                DXGI_FORMAT_R16G16_UINT, 0, 1, picture.slice, 1);
  ComPtr<ID3D11ShaderResourceView> srvs[2];
  const UnpackParams params{width, height, chromaWidth, chromaHeight, shift, {}};
  if (FAILED(m_device->CreateShaderResourceView(picture.texture, &luma, &srvs[0])) ||
      FAILED(m_device->CreateShaderResourceView(picture.texture, &chroma, &srvs[1])) ||
      !EnsureTexture(textures[0], width, height, DXGI_FORMAT_R16_UINT, true) ||
      !EnsureTexture(textures[1], chromaWidth, chromaHeight, DXGI_FORMAT_R16_UINT, true) ||
      !EnsureTexture(textures[2], chromaWidth, chromaHeight, DXGI_FORMAT_R16_UINT, true) ||
      !UpdateConstants(m_unpackParams.Get(), params))
    return false;

  ID3D11ShaderResourceView* views[] = {srvs[0].Get(), srvs[1].Get()};
  ID3D11UnorderedAccessView* uavs[] = {textures[0].uav.Get(), textures[1].uav.Get(),
                                       textures[2].uav.Get()};
  m_deferred->CSSetShader(m_unpackCs.Get(), nullptr, 0);
  m_deferred->CSSetConstantBuffers(0, 1, m_unpackParams.GetAddressOf());
  m_deferred->CSSetShaderResources(0, 2, views);
  m_deferred->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
  Dispatch(width, height, 16);
  UnbindCompute();
  return true;
}

bool CDvGpuCarrier::UploadTables(const DvTables* tables, const DvReshapeLut* lut)
{
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(m_deferred->Map(m_tables.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return false;
  auto* out = static_cast<std::int32_t*>(mapped.pData);
  std::fill_n(out, TABLE_ENTRIES, 0);
  for (unsigned c = 0; c < 3; ++c)
  {
    if (tables)
    {
      std::copy(tables->predict[c].begin(), tables->predict[c].end(),
                out + TABLE_PREDICT + c * BL_CODES);
      std::copy(tables->residualQ16[c].begin(), tables->residualQ16[c].end(),
                out + TABLE_RESIDUAL + c * BL_CODES);
    }
    if (lut)
      std::copy((*lut)[c].begin(), (*lut)[c].end(), out + TABLE_RESHAPE + c * BL_CODES);
  }
  m_deferred->Unmap(m_tables.Get(), 0);
  return true;
}

template<typename T>
bool CDvGpuCarrier::UpdateConstants(ID3D11Buffer* buffer, const T& data)
{
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(m_deferred->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return false;
  std::memcpy(mapped.pData, &data, sizeof(T));
  m_deferred->Unmap(buffer, 0);
  return true;
}

void CDvGpuCarrier::Dispatch(unsigned width, unsigned height, unsigned group)
{
  m_deferred->Dispatch((width + group - 1) / group, (height + group - 1) / group, 1);
}

void CDvGpuCarrier::UnbindCompute()
{
  ID3D11ShaderResourceView* srvs[8] = {};
  ID3D11UnorderedAccessView* uavs[3] = {};
  m_deferred->CSSetShaderResources(0, 8, srvs);
  m_deferred->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
}

bool CDvGpuCarrier::RecordCompose(const DvTables& tables, bool useEl)
{
  const unsigned width = m_canvasWidth;
  const unsigned height = m_canvasHeight;

  if (useEl)
  {
    m_deferred->CSSetShader(m_elVerticalCs.Get(), nullptr, 0);
    for (int p = 0; p < 3; ++p)
    {
      const Texture& in = m_el[p];
      const ElParams params{in.width, in.height, p == 0 ? 1u : 0u, 0};
      if (!EnsureTexture(m_elVertical[p], in.width, in.height * 2, DXGI_FORMAT_R16_UINT, true) ||
          !UpdateConstants(m_elParams[p].Get(), params))
        return false;
      m_deferred->CSSetConstantBuffers(0, 1, m_elParams[p].GetAddressOf());
      m_deferred->CSSetShaderResources(0, 1, in.srv.GetAddressOf());
      m_deferred->CSSetUnorderedAccessViews(0, 1, m_elVertical[p].uav.GetAddressOf(), nullptr);
      Dispatch(in.width, in.height * 2, 8);
      UnbindCompute();
    }
  }

  const LumaParams luma{width, height, useEl ? 1u : 0u, useEl ? m_elVertical[0].width : 0u};
  if (!UpdateConstants(m_lumaParams.Get(), luma))
    return false;
  {
    ID3D11ShaderResourceView* srvs[] = {
        m_src[0].srv.Get(), useEl ? m_elVertical[0].srv.Get() : nullptr, m_tablesSrv.Get()};
    m_deferred->CSSetShader(m_composeLumaCs.Get(), nullptr, 0);
    m_deferred->CSSetConstantBuffers(0, 1, m_lumaParams.GetAddressOf());
    m_deferred->CSSetShaderResources(0, 3, srvs);
    m_deferred->CSSetUnorderedAccessViews(0, 1, m_out[0].uav.GetAddressOf(), nullptr);
    Dispatch(width, height, 16);
    UnbindCompute();
  }

  ChromaParams chroma{};
  chroma.chromaWidth = width / 2;
  chroma.chromaHeight = height / 2;
  chroma.lumaWidth = width;
  chroma.useEl = useEl ? 1 : 0;
  chroma.elWidth = useEl ? m_elVertical[1].width : 0;
  chroma.mmrMask = (tables.mmr[1] ? 2u : 0u) | (tables.mmr[2] ? 4u : 0u);
  chroma.orderCb = tables.mmrOrder[1];
  chroma.orderCr = tables.mmrOrder[2];
  for (int c = 0; c < 3; ++c)
  {
    chroma.pivotLow[c] = tables.pivotLow[c];
    chroma.pivotHigh[c] = tables.pivotHigh[c];
  }
  PutDouble(std::ldexp(1.0, -static_cast<int>(tables.denom + 4)), chroma.scale);
  for (int c = 1; c <= 2; ++c)
  {
    // Both conversions are exact: |constant|, |coefficient| < 2^40.
    std::uint32_t* out = &chroma.mmr[(c - 1) * 11][0];
    PutDouble(static_cast<double>(tables.mmrConstant[c]) * 1048576.0, out);
    for (unsigned t = 0; t < MMR_FEATURES; ++t)
      PutDouble(static_cast<double>(tables.mmrCoef[c][t]), out + 2 * (t + 1));
  }
  if (!UpdateConstants(m_chromaParams.Get(), chroma))
    return false;
  {
    ID3D11ShaderResourceView* srvs[] = {m_src[0].srv.Get(),
                                        m_src[1].srv.Get(),
                                        m_src[2].srv.Get(),
                                        useEl ? m_elVertical[1].srv.Get() : nullptr,
                                        useEl ? m_elVertical[2].srv.Get() : nullptr,
                                        m_tablesSrv.Get()};
    ID3D11UnorderedAccessView* uavs[] = {m_out[1].uav.Get(), m_out[2].uav.Get()};
    m_deferred->CSSetShader(m_composeChromaCs.Get(), nullptr, 0);
    m_deferred->CSSetConstantBuffers(0, 1, m_chromaParams.GetAddressOf());
    m_deferred->CSSetShaderResources(0, 6, srvs);
    m_deferred->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    Dispatch(width / 2, height / 2, 8);
    UnbindCompute();
  }
  return true;
}

bool CDvGpuCarrier::RecordPack(const DvCarrierParams& carrier,
                               ID3D11Texture2D* target,
                               unsigned width,
                               unsigned height)
{
  if (!carrier.packets || carrier.packets->empty() || carrier.packets->size() > MAX_PACKETS ||
      !EnsureTexture(m_packed, width, height, DXGI_FORMAT_R32_UINT, true))
    return false;

  const Texture* planes = m_out;
  const DvRect& dst = carrier.scaled;
  if (!dst.IsEmpty())
  {
    const ScaleParams scale{width,    height,  m_canvasWidth,        m_canvasHeight,
                            dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top};
    if (dst.right > width || dst.bottom > height ||
        ((dst.left | dst.top | dst.right | dst.bottom) & 1) ||
        !EnsureTexture(m_scaled[0], width, height, DXGI_FORMAT_R16_UINT, true) ||
        !EnsureTexture(m_scaled[1], width / 2, height / 2, DXGI_FORMAT_R16_UINT, true) ||
        !EnsureTexture(m_scaled[2], width / 2, height / 2, DXGI_FORMAT_R16_UINT, true) ||
        !UpdateConstants(m_scaleParams.Get(), scale))
      return false;
    ID3D11ShaderResourceView* srvs[] = {m_out[0].srv.Get(), m_out[1].srv.Get(), m_out[2].srv.Get(),
                                        m_scaleKernel.Get()};
    ID3D11UnorderedAccessView* uavs[] = {m_scaled[0].uav.Get(), m_scaled[1].uav.Get(),
                                         m_scaled[2].uav.Get()};
    m_deferred->CSSetShader(m_scaleCs.Get(), nullptr, 0);
    m_deferred->CSSetConstantBuffers(0, 1, m_scaleParams.GetAddressOf());
    m_deferred->CSSetShaderResources(0, 4, srvs);
    m_deferred->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    Dispatch(width, height, 16);
    UnbindCompute();
    planes = m_scaled;
  }

  PackParams params{};
  params.width = width;
  params.height = height;
  params.bits = static_cast<std::uint32_t>(carrier.packets->size() * 3 * PACKET_BYTES * 8);
  params.mask[0] = carrier.mask.left;
  params.mask[1] = carrier.mask.top;
  params.mask[2] = carrier.mask.right;
  params.mask[3] = carrier.mask.bottom;
  std::memcpy(params.packets, carrier.packets->data(), carrier.packets->size() * PACKET_BYTES);
  if (!UpdateConstants(m_packParams.Get(), params))
    return false;

  ID3D11ShaderResourceView* srvs[] = {planes[0].srv.Get(), planes[1].srv.Get(),
                                      planes[2].srv.Get()};
  m_deferred->CSSetShader(m_packCs.Get(), nullptr, 0);
  m_deferred->CSSetConstantBuffers(0, 1, m_packParams.GetAddressOf());
  m_deferred->CSSetShaderResources(0, 3, srvs);
  m_deferred->CSSetUnorderedAccessViews(0, 1, m_packed.uav.GetAddressOf(), nullptr);
  Dispatch(width, height, 16);
  UnbindCompute();

  ComPtr<ID3D11RenderTargetView> rtv;
  if (FAILED(m_device->CreateRenderTargetView(target, nullptr, &rtv)))
    return false;
  m_deferred->PSSetShaderResources(0, 1, m_packed.srv.GetAddressOf());
  DrawFullscreen(m_copyPs.Get(), rtv.Get(), width, height);
  return true;
}

void CDvGpuCarrier::DrawFullscreen(ID3D11PixelShader* ps,
                                   ID3D11RenderTargetView* rtv,
                                   unsigned width,
                                   unsigned height)
{
  const CD3D11_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
  m_deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_deferred->VSSetShader(m_fullscreenVs.Get(), nullptr, 0);
  m_deferred->PSSetShader(ps, nullptr, 0);
  m_deferred->RSSetState(m_rasterizer.Get());
  m_deferred->RSSetViewports(1, &viewport);
  m_deferred->OMSetRenderTargets(1, &rtv, nullptr);
  m_deferred->Draw(3, 0);

  ID3D11ShaderResourceView* srvs[2] = {};
  m_deferred->PSSetShaderResources(0, 2, srvs);
  m_deferred->OMSetRenderTargets(0, nullptr, nullptr);
}

void CDvGpuCarrier::Abandon()
{
  ComPtr<ID3D11CommandList> list;
  m_deferred->FinishCommandList(FALSE, &list);
}

bool CDvGpuCarrier::Submit()
{
  ComPtr<ID3D11CommandList> list;
  if (FAILED(m_deferred->FinishCommandList(FALSE, &list)))
    return false;
  m_immediate->ExecuteCommandList(list.Get(), TRUE);
  return m_device->GetDeviceRemovedReason() == S_OK;
}

namespace
{

struct TextureSize
{
  unsigned width;
  unsigned height;
};

// The size of a texture the carrier can write: single-sample BGRA, one slice.
std::optional<TextureSize> TargetSize(ID3D11Texture2D* target)
{
  if (!target)
    return {};
  D3D11_TEXTURE2D_DESC desc;
  target->GetDesc(&desc);
  if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.ArraySize != 1 ||
      desc.SampleDesc.Count != 1)
    return {};
  return TextureSize{desc.Width, desc.Height};
}

bool PictureShapeOk(const DvPicture& p)
{
  return p.texture && p.width >= 2 && p.height >= 2 && !(p.width & 1) && !(p.height & 1);
}

bool EnhancementShapeOk(const DvPicture& el, unsigned width, unsigned height)
{
  return el.texture && el.width == width / 2 && el.height == height / 2;
}

} // namespace

bool CDvGpuCarrier::PackReshape(const DvPicture& bl,
                                unsigned padLeft,
                                unsigned padTop,
                                const DvReshapeLut& lut,
                                const DvCarrierParams& params,
                                ID3D11Texture2D* target)
{
  const std::optional<TextureSize> targetSize = TargetSize(target);
  if (!m_device || !targetSize)
    return false;
  const bool scaled = !params.scaled.IsEmpty();
  const unsigned width = scaled ? bl.width : targetSize->width;
  const unsigned height = scaled ? bl.height : targetSize->height;
  if (!PictureShapeOk(bl) || padLeft + bl.width > width || padTop + bl.height > height ||
      (padLeft & 1) || (padTop & 1) || !EnsureCanvas(width, height) || !RecordUnpack(bl, m_src, 2))
    return false;

  const ReshapeParams reshape{width, height, padLeft, padTop, bl.width, bl.height, {}};
  if (!UploadTables(nullptr, &lut) || !UpdateConstants(m_reshapeParams.Get(), reshape))
  {
    Abandon();
    return false;
  }
  ID3D11ShaderResourceView* srvs[] = {m_src[0].srv.Get(), m_src[1].srv.Get(), m_src[2].srv.Get(),
                                      m_tablesSrv.Get()};
  ID3D11UnorderedAccessView* uavs[] = {m_out[0].uav.Get(), m_out[1].uav.Get(), m_out[2].uav.Get()};
  m_deferred->CSSetShader(m_reshapeCs.Get(), nullptr, 0);
  m_deferred->CSSetConstantBuffers(0, 1, m_reshapeParams.GetAddressOf());
  m_deferred->CSSetShaderResources(0, 4, srvs);
  m_deferred->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
  Dispatch(width, height, 16);
  UnbindCompute();

  if (!RecordPack(params, target, targetSize->width, targetSize->height))
  {
    Abandon();
    return false;
  }
  return Submit();
}

bool CDvGpuCarrier::Compose(const DvPicture& bl, const DvPicture* el, const DvTables& tables)
{
  if (!m_device || !m_composerAvailable || !PictureShapeOk(bl) ||
      (el && !EnhancementShapeOk(*el, bl.width, bl.height)) || !EnsureCanvas(bl.width, bl.height) ||
      !RecordUnpack(bl, m_src, 2))
    return false;

  // Without a residual the enhancement layer contributes nothing.
  const bool useEl = el && tables.residual;
  if (useEl && !RecordUnpack(*el, m_el, 0))
    return false;
  if (!UploadTables(&tables, nullptr) || !RecordCompose(tables, useEl))
  {
    Abandon();
    return false;
  }
  return true;
}

bool CDvGpuCarrier::PackCompose(const DvPicture& bl,
                                const DvPicture* el,
                                const DvTables& tables,
                                const DvCarrierParams& params,
                                ID3D11Texture2D* target)
{
  const std::optional<TextureSize> size = TargetSize(target);
  if (!size ||
      (params.scaled.IsEmpty() && (bl.width != size->width || bl.height != size->height)) ||
      !Compose(bl, el, tables))
    return false;
  if (!RecordPack(params, target, size->width, size->height))
  {
    Abandon();
    return false;
  }
  return Submit();
}

bool CDvGpuCarrier::CompositeGui(ID3D11Texture2D* frame,
                                 std::size_t packetCount,
                                 ID3D11Texture2D* gui,
                                 ID3D11Texture2D* target,
                                 const DvGuiTransform& transform)
{
  const std::optional<TextureSize> size = TargetSize(frame);
  const std::optional<TextureSize> targetSize = TargetSize(target);
  if (!m_device || !gui || packetCount > MAX_PACKETS || !size || !targetSize ||
      size->width != targetSize->width || size->height != targetSize->height || frame == target)
    return false;
  const unsigned width = size->width;
  const unsigned height = size->height;

  D3D11_TEXTURE2D_DESC guiDesc;
  gui->GetDesc(&guiDesc);
  ComPtr<ID3D11ShaderResourceView> frameView, guiView;
  ComPtr<ID3D11RenderTargetView> rtv;
  if (FAILED(m_device->CreateShaderResourceView(frame, nullptr, &frameView)) ||
      FAILED(m_device->CreateShaderResourceView(gui, nullptr, &guiView)) ||
      FAILED(m_device->CreateRenderTargetView(target, nullptr, &rtv)))
    return false;

  GuiParams params{};
  for (int r = 0; r < 3; ++r)
  {
    for (int c = 0; c < 3; ++c)
    {
      params.toSource[r][c] = transform.toSource[r][c];
      params.toCodes[r][c] = transform.toCodes[r][c];
    }
    params.offset[r] = transform.offset[r];
  }
  params.width = width;
  params.bits = static_cast<std::uint32_t>(packetCount * 3 * PACKET_BYTES * 8);
  params.guiWidth = guiDesc.Width;
  params.guiHeight = guiDesc.Height;
  if (!UpdateConstants(m_guiParams.Get(), params))
  {
    Abandon();
    return false;
  }

  ID3D11ShaderResourceView* srvs[] = {frameView.Get(), guiView.Get()};
  m_deferred->PSSetConstantBuffers(0, 1, m_guiParams.GetAddressOf());
  m_deferred->PSSetShaderResources(0, 2, srvs);
  DrawFullscreen(m_guiPs.Get(), rtv.Get(), width, height);
  return Submit();
}

} // namespace DVTUNNEL
