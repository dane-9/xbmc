/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DoviTunnel.h"

#include "DvGpuCarrier.h"
#include "DvMappingTables.h"
#include "DvRpuTransport.h"
#include "DvTunnelStats.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DXVA.h"
#include "cores/VideoPlayer/VideoRenderers/VideoShaders/ConversionMatrix.h"
#include "cores/VideoPlayer/VideoRenderers/VideoShaders/ConvolutionKernels.h"
#include "cores/VideoPlayer/VideoRenderers/windows/RendererBase.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include <wrl/client.h>

extern "C"
{
#include <libavutil/pixfmt.h>
}

using namespace DVTUNNEL;

namespace
{

void LogLine(DvLogLevel level, const std::string& line)
{
  switch (level)
  {
    case DvLogLevel::DEBUG:
      CLog::Log(LOGDEBUG, "{}", line);
      break;
    case DvLogLevel::INFO:
      CLog::Log(LOGINFO, "{}", line);
      break;
    case DvLogLevel::WARNING:
      CLog::Log(LOGWARNING, "{}", line);
      break;
  }
}

void Bt709ToBt2020(double (&m)[3][3])
{
  CConvertMatrix convert;
  convert.SetSourceColorPrimaries(AVCOL_PRI_BT709).SetDestinationColorPrimaries(AVCOL_PRI_BT2020);
  // GetPrimMat returns the matrix transposed for column-major shader use.
  const Matrix3 prim = convert.GetPrimMat();
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      m[r][c] = prim[c][r];
}

// Kodi's kernel for a scaling method as a DvScaleKernel. The video
// processor's methods (automatic, DXVA, super resolution) use Kodi's
// Catmull-Rom.
DvScaleKernel KodiScaleKernel(ESCALINGMETHOD method)
{
  switch (method)
  {
    case VS_SCALINGMETHOD_LINEAR:
      return LinearKernel();
    case VS_SCALINGMETHOD_NEAREST:
      return NearestKernel();
    case VS_SCALINGMETHOD_CUBIC_MITCHELL:
    case VS_SCALINGMETHOD_LANCZOS2:
    case VS_SCALINGMETHOD_SPLINE36_FAST:
    case VS_SCALINGMETHOD_LANCZOS3_FAST:
    case VS_SCALINGMETHOD_SPLINE36:
    case VS_SCALINGMETHOD_LANCZOS3:
      break;
    default:
      method = VS_SCALINGMETHOD_CUBIC_CATMULL;
      break;
  }
  CConvolutionKernel kodi(method, 2 * SCALE_PHASES);
  return ScaleKernelFromKodi(kodi.GetFloatPixels(), method == VS_SCALINGMETHOD_SPLINE36 ||
                                                        method == VS_SCALINGMETHOD_LANCZOS3);
}

// The largest rectangle with the picture's aspect ratio centred in the canvas,
// on even coordinates.
DvRect FitPicture(unsigned width, unsigned height, unsigned canvasWidth, unsigned canvasHeight)
{
  unsigned w = canvasWidth;
  unsigned h = canvasHeight;
  if (std::uint64_t{width} * canvasHeight >= std::uint64_t{height} * canvasWidth)
    h = std::min(canvasHeight, static_cast<unsigned>(std::lround(static_cast<double>(height) *
                                                                 canvasWidth / width / 2)) *
                                   2);
  else
    w = std::min(canvasWidth, static_cast<unsigned>(std::lround(static_cast<double>(width) *
                                                                canvasHeight / height / 2)) *
                                  2);
  const unsigned left = ((canvasWidth - w) / 2) & ~1u;
  const unsigned top = ((canvasHeight - h) / 2) & ~1u;
  return {left, top, left + w, top + h};
}

// A usable enhancement layer is a decoded P010 picture at half the picture
// size. The decoder's buffer keeps the texture alive while the picture is used.
std::optional<DvPicture> EnhancementView(const CRenderBuffer& buffer,
                                         unsigned width,
                                         unsigned height)
{
  auto* picture = dynamic_cast<DXVA::CVideoBuffer*>(buffer.doviEnhancement.get());
  Microsoft::WRL::ComPtr<ID3D11Resource> resource;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (!picture || FAILED(picture->GetResource(&resource)) || FAILED(resource.As(&texture)))
    return {};
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  const unsigned w = width / 2;
  const unsigned h = height / 2;
  if (desc.Format != DXGI_FORMAT_P010 || desc.Width < w || desc.Height < h)
    return {};
  return DvPicture{texture.Get(), picture->GetIdx(), w, h};
}

} // namespace

bool IsDolbyVisionTunnelMetadata(const void* metadata,
                                 std::size_t metadataSize,
                                 int profile,
                                 int compatibilityId)
{
  return ClassifyMetadata(metadata, metadataSize, profile, compatibilityId) == DvVerdict::ACCEPTED;
}

bool DolbyVisionMetadataNeedsComposer(const void* metadata, std::size_t metadataSize)
{
  return !ReadSingleLayerMapping(metadata, metadataSize);
}

struct CDoviTunnel::Impl
{
  Impl() : stats(LogLine) {}

  bool Skip(DvSkip reason)
  {
    stats.Skip(reason);
    return false;
  }

  CDvGpuCarrier carrier;
  CDvTunnelStats stats;
  double m709[3][3] = {};

  // Per-picture scratch, kept to avoid large stack frames on the render thread.
  DvMapping mapping;
  DvTables tables;
  DvReshapeLut lut;
  std::vector<DvPacket> packets;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> lastFrame;
  std::size_t lastPacketCount = 0;
  std::optional<DvGuiTransform> gui;
  ESCALINGMETHOD scalingMethod = VS_SCALINGMETHOD_CUBIC_CATMULL;
};

CDoviTunnel::CDoviTunnel() = default;

CDoviTunnel::~CDoviTunnel()
{
  Reset();
}

bool CDoviTunnel::Initialize(ID3D11Device* device)
{
  Reset();
  m_impl = std::make_unique<Impl>();
  Bt709ToBt2020(m_impl->m709);

  HRESULT hr = S_OK;
  std::string message;
  if (!m_impl->carrier.Initialize(device, hr, message))
  {
    CLog::Log(LOGWARNING, "Dolby Vision tunnel: D3D11 packing initialization failed ({:#x}): {}",
              static_cast<std::uint32_t>(hr), message);
    Reset();
    return false;
  }

  const CDvGpuCarrier& carrier = m_impl->carrier;
  if (carrier.ComposerAvailable())
    CLog::Log(LOGDEBUG, "Dolby Vision tunnel: D3D11 compute packing initialized.");
  else
    CLog::Log(LOGWARNING, "Dolby Vision tunnel: the composer is unavailable {}.",
              carrier.ComposerStatus());
  return true;
}

bool CDoviTunnel::CanCompose() const
{
  return m_impl && m_impl->carrier.ComposerAvailable();
}

void CDoviTunnel::Reset()
{
  if (!m_impl)
    return;
  m_impl->stats.Finish();
  m_impl.reset();
}

void CDoviTunnel::SetScalingMethod(ESCALINGMETHOD method)
{
  if (!m_impl || m_impl->scalingMethod == method)
    return;
  m_impl->scalingMethod = method;
  if (!m_impl->carrier.SetScaleKernel(KodiScaleKernel(method)))
    CLog::Log(LOGWARNING, "Dolby Vision tunnel: cannot use scaling method {}.",
              static_cast<int>(method));
}

bool CDoviTunnel::Render(const CRenderBuffer& buffer,
                         unsigned width,
                         unsigned height,
                         ID3D11Texture2D* decoded,
                         unsigned slice,
                         CD3DTexture& target)
{
  if (!m_impl)
    return false;
  Impl& d = *m_impl;
  if (!decoded)
    return d.Skip(DvSkip::PICTURE_UNAVAILABLE);
  const DoviRpu* rpu = buffer.doviRpu.get();
  if (!rpu || !IsSupportedProfile(rpu->profile, rpu->compatibilityId))
    return d.Skip(DvSkip::UNSUPPORTED_PROFILE);

  ID3D11Texture2D* texture = target.Get();
  const unsigned canvasWidth = target.GetWidth();
  const unsigned canvasHeight = target.GetHeight();
  if (width == 0 || height == 0)
    return d.Skip(DvSkip::UNEXPECTED_PICTURE);
  const DvRect picture = FitPicture(width, height, canvasWidth, canvasHeight);
  const unsigned pictureWidth = picture.right - picture.left;
  const unsigned pictureHeight = picture.bottom - picture.top;

  const void* blob = rpu->metadata.data();
  const std::size_t blobSize = rpu->metadata.size();
  const DvPicture bl{decoded, slice, width, height};
  std::optional<DvPicture> elView;
  const DvPicture* el = nullptr;

  // Reshape mode places an unscaled picture directly on the canvas; anything
  // else is composed at the picture's size and resampled into place.
  std::optional<DvMapping> mapping = ReadSingleLayerMapping(blob, blobSize);
  const bool reshape = mapping.has_value();
  const bool scaled = pictureWidth != width || pictureHeight != height ||
                      (!reshape && (width != canvasWidth || height != canvasHeight));
  if (reshape)
  {
    d.mapping = *mapping;
    d.stats.Mode({});
    if (!BuildTables(d.mapping, d.tables) || !BuildReshapeLut(d.tables, d.lut))
      return d.Skip(DvSkip::COEFFICIENTS_OUT_OF_RANGE);
  }
  else
  {
    mapping = ReadComposerMapping(blob, blobSize);
    if (!mapping)
      return d.Skip(DvSkip::METADATA_UNREADABLE);
    d.mapping = *mapping;
    const bool residual = d.mapping.residual;
    if (residual && rpu->profile != 7)
      return d.Skip(DvSkip::ENHANCEMENT_OUTSIDE_PROFILE7);
    if (residual)
    {
      elView = EnhancementView(buffer, width, height);
      if (elView)
        el = &*elView;
      else
      {
        d.stats.EnhancementMiss();
        d.mapping.residual = false;
      }
    }
    if (!BuildTables(d.mapping, d.tables))
      return d.Skip(DvSkip::COEFFICIENTS_OUT_OF_RANGE);
    d.stats.Mode({false, residual, d.tables.AnyChromaMmr()});
  }

  std::optional<DvDisplayMetadata> metadata = SerializeDisplayMetadata(blob, blobSize);
  // The GUI may draw into letterbox areas, so the TV must treat the whole
  // canvas as active.
  if (!metadata || !ClearLevel5Payloads(metadata->bytes))
    return d.Skip(DvSkip::TRANSPORT_REJECTED);
  std::optional<std::vector<DvPacket>> packets = Packetize(metadata->bytes);
  if (!packets)
    return d.Skip(DvSkip::PACKETIZATION_FAILED);
  d.packets = std::move(*packets);

  // Level-5 offsets often describe a different encode of the film, so only
  // the canvas outside the picture is blanked.
  const DvCarrierParams params{picture, &d.packets, scaled ? picture : DvRect{}};
  const unsigned padLeft = scaled ? 0 : picture.left;
  const unsigned padTop = scaled ? 0 : picture.top;
  const bool packed = reshape ? d.carrier.PackReshape(bl, padLeft, padTop, d.lut, params, texture)
                              : d.carrier.PackCompose(bl, el, d.tables, params, texture);
  if (!packed)
    return d.Skip(DvSkip::GPU_PACKING_FAILED);

  d.stats.Packed();
  d.lastFrame = texture;
  d.lastPacketCount = d.packets.size();
  d.gui.reset();
  if (metadata->colour.colorSpace == 2 || buffer.primaries == AVCOL_PRI_BT2020)
    d.gui = BuildGuiTransform(metadata->colour, d.m709);
  return true;
}

bool CDoviTunnel::CompositeGUI(ID3D11Texture2D* gui, ID3D11Texture2D* target)
{
  if (!m_impl || !m_impl->lastFrame || !m_impl->gui)
    return false;
  return m_impl->carrier.CompositeGui(m_impl->lastFrame.Get(), m_impl->lastPacketCount, gui, target,
                                      *m_impl->gui);
}
