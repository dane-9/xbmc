/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include "cores/VideoSettings.h"
#include "guilib/D3DResource.h"

#include <cstddef>
#include <cstdint>
#include <memory>

class CRenderBuffer;

/*!
 * \brief Checks whether the tunnel can render a stream's Dolby Vision metadata.
 *
 * \param metadata the AV_FRAME_DATA_DOVI_METADATA side data of a picture
 * \param metadataSize size of metadata in bytes
 * \param profile Dolby Vision profile of the stream
 * \param compatibilityId base-layer signal compatibility ID of the stream
 * \return true for a supported profile with a mapping the tunnel computes
 */
bool IsDolbyVisionTunnelMetadata(const void* metadata,
                                 std::size_t metadataSize,
                                 int profile,
                                 int compatibilityId);

/*!
 * \brief True when the mapping needs the GPU composer: an enhancement layer
 * or MMR chroma prediction.
 */
bool DolbyVisionMetadataNeedsComposer(const void* metadata, std::size_t metadataSize);

/*!
 * \brief Packs Dolby Vision pictures and their metadata into 8-bit carrier
 * frames for TV-led output.
 *
 * Each picture is reshaped or composed to 12-bit PQ, scaled to the carrier
 * frame, and packed with its display management metadata. Metadata layout per
 * ETSI GS CCM 001, transfer per SMPTE ST 2084.
 */
class CDoviTunnel
{
public:
  CDoviTunnel();
  ~CDoviTunnel();

  CDoviTunnel(const CDoviTunnel&) = delete;
  CDoviTunnel& operator=(const CDoviTunnel&) = delete;

  bool Initialize(ID3D11Device* device);

  /*!
   * \brief True when the GPU composer is available on the device.
   */
  bool CanCompose() const;

  /*!
   * \brief Blends Kodi's GUI over the last rendered frame into target, in the
   * frame's source colour space.
   *
   * \return false when no frame was rendered or its colour space is not
   *         supported for blending
   */
  bool CompositeGUI(ID3D11Texture2D* gui, ID3D11Texture2D* target);

  /*!
   * \brief Ends playback: logs the playback summary and releases the GPU
   * resources. Initialize must be called before the next Render.
   */
  void Reset();

  /*!
   * \brief Sets Kodi's video scaling method, used when the picture is upscaled.
   */
  void SetScalingMethod(ESCALINGMETHOD method);

  /*!
   * \brief Renders one picture into a carrier frame.
   *
   * The picture is scaled to fit target with its aspect ratio kept.
   *
   * \param buffer the render buffer holding the picture's metadata and
   *        enhancement layer
   * \param width picture width
   * \param height picture height
   * \param decoded the decoder's P010 texture array on the tunnel's device, or
   *        null when the picture is unavailable
   * \param slice the picture's index in decoded
   * \param target the carrier frame
   * \return false when the picture was skipped; the reason is logged once
   */
  bool Render(const CRenderBuffer& buffer,
              unsigned width,
              unsigned height,
              ID3D11Texture2D* decoded,
              unsigned slice,
              CD3DTexture& target);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
