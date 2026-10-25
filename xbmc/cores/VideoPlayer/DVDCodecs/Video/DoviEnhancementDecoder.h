/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDVideoCodec.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <utility>

class CDVDStreamInfo;
class CVideoBuffer;
struct AVBSFContext;
struct AVCodecContext;

/*!
 * \brief Decodes the Dolby Vision Profile 7 enhancement layer with Kodi's
 * hardware decoder.
 *
 * The layer travels inside the base-layer access units as UNSPEC63 NAL units.
 * Its pictures are matched to base-layer pictures by the packet timestamps they
 * were sent with.
 */
class CDoviEnhancementDecoder : public ICallbackHWAccel
{
public:
  CDoviEnhancementDecoder();
  ~CDoviEnhancementDecoder() override;
  CDoviEnhancementDecoder(const CDoviEnhancementDecoder&) = delete;
  CDoviEnhancementDecoder& operator=(const CDoviEnhancementDecoder&) = delete;

  /*!
   * \brief Creates a decoder for a stream's enhancement layer.
   *
   * \return the decoder, or null unless the stream is HEVC Profile 7 with an
   *         enhancement layer that a hardware decoder can decode
   */
  static std::unique_ptr<CDoviEnhancementDecoder> Create(const CDVDStreamInfo& hints);
  bool Open(const CDVDStreamInfo& hints);

  /*!
   * \brief Decodes the enhancement layer of one base-layer packet.
   *
   * \param data the base-layer packet
   * \param size size of data in bytes
   * \param pts presentation timestamp of the packet
   * \param dts decoding timestamp of the packet
   */
  void Send(const std::uint8_t* data, int size, std::int64_t pts, std::int64_t dts);

  /*!
   * \brief Takes the enhancement-layer picture decoded for pts.
   *
   * Pictures decoded for earlier timestamps are dropped.
   *
   * \return the picture, or null if none was decoded for pts
   */
  std::shared_ptr<CVideoBuffer> Take(std::int64_t pts);
  void Flush();

  IHardwareDecoder* GetHWAccel() override { return m_hardware; }
  bool GetPictureCommon(VideoPicture* picture) override { return true; }

private:
  static enum AVPixelFormat GetFormat(AVCodecContext* avctx, const enum AVPixelFormat* formats);
  void Receive();

  // The hardware decoder reports to its own process info, not the stream's.
  std::unique_ptr<CProcessInfo> m_processInfo;
  std::unique_ptr<CDVDStreamInfo> m_hints;
  AVBSFContext* m_filter = nullptr;
  AVCodecContext* m_decoder = nullptr;
  IHardwareDecoder* m_hardware = nullptr;
  std::deque<std::pair<std::int64_t, std::shared_ptr<CVideoBuffer>>> m_frames;
};
