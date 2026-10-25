/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DoviEnhancementDecoder.h"

#include "DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "utils/log.h"

#include <cstring>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
}

namespace
{
// Pictures waiting for their base layer hold decoder surfaces, so keep few.
constexpr std::size_t MAXIMUM_QUEUED = 8;
} // namespace

CDoviEnhancementDecoder::CDoviEnhancementDecoder() : m_processInfo(CProcessInfo::CreateInstance())
{
}

CDoviEnhancementDecoder::~CDoviEnhancementDecoder()
{
  m_frames.clear();
  av_bsf_free(&m_filter);
  avcodec_free_context(&m_decoder);
  if (m_hardware)
    m_hardware->Release();
}

enum AVPixelFormat CDoviEnhancementDecoder::GetFormat(AVCodecContext* avctx,
                                                      const enum AVPixelFormat* formats)
{
  auto* self = static_cast<CDoviEnhancementDecoder*>(static_cast<ICallbackHWAccel*>(avctx->opaque));
  if (self->m_hardware)
  {
    self->m_hardware->Release();
    self->m_hardware = nullptr;
    avctx->get_buffer2 = avcodec_default_get_buffer2;
    avctx->slice_flags = 0;
    av_buffer_unref(&avctx->hw_frames_ctx);
  }

  for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
  {
    for (const std::string& id : CDVDFactoryCodec::GetHWAccels())
    {
      IHardwareDecoder* decoder = CDVDFactoryCodec::CreateVideoCodecHWAccel(
          id, *self->m_hints, *self->m_processInfo, *format);
      if (!decoder)
        continue;
      if (decoder->Open(avctx, avctx, *format))
      {
        self->m_hardware = decoder;
        return *format;
      }
      decoder->Release();
    }
  }

  CLog::Log(LOGWARNING, "Dolby Vision enhancement layer: no hardware decoder; composing without "
                        "the enhancement layer");
  return AV_PIX_FMT_NONE;
}

std::unique_ptr<CDoviEnhancementDecoder> CDoviEnhancementDecoder::Create(
    const CDVDStreamInfo& hints)
{
  if (hints.codec != AV_CODEC_ID_HEVC || hints.dovi.dv_profile != 7 || !hints.dovi.el_present_flag)
    return nullptr;
  auto decoder = std::make_unique<CDoviEnhancementDecoder>();
  if (decoder->Open(hints))
    return decoder;
  CLog::Log(LOGWARNING, "Dolby Vision enhancement layer: decoder could not be opened");
  return nullptr;
}

bool CDoviEnhancementDecoder::Open(const CDVDStreamInfo& hints)
{
  const AVBitStreamFilter* split = av_bsf_get_by_name("dovi_split");
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
  if (!split || !codec || av_bsf_alloc(split, &m_filter) < 0)
    return false;

  AVCodecParameters* in = m_filter->par_in;
  in->codec_type = AVMEDIA_TYPE_VIDEO;
  in->codec_id = AV_CODEC_ID_HEVC;
  in->width = hints.width;
  in->height = hints.height;
  if (hints.extradata)
  {
    in->extradata = static_cast<std::uint8_t*>(
        av_mallocz(hints.extradata.GetSize() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!in->extradata)
      return false;
    std::memcpy(in->extradata, hints.extradata.GetData(), hints.extradata.GetSize());
    in->extradata_size = static_cast<int>(hints.extradata.GetSize());
  }
  if (av_opt_set(m_filter->priv_data, "mode", "el", 0) < 0 || av_bsf_init(m_filter) < 0)
    return false;

  m_hints = std::make_unique<CDVDStreamInfo>(hints, false);
  m_hints->width = m_filter->par_out->width;
  m_hints->height = m_filter->par_out->height;

  m_decoder = avcodec_alloc_context3(codec);
  if (!m_decoder || avcodec_parameters_to_context(m_decoder, m_filter->par_out) < 0)
    return false;
  // Kodi's hardware decoders need single-threaded decoding.
  m_decoder->thread_count = 1;
  m_decoder->opaque = static_cast<ICallbackHWAccel*>(this);
  m_decoder->get_format = GetFormat;
  if (avcodec_open2(m_decoder, codec, nullptr) < 0)
    return false;

  CLog::Log(LOGINFO, "Dolby Vision enhancement layer: decoding {}x{} layer for {}x{} base layer",
            m_filter->par_out->width, m_filter->par_out->height, hints.width, hints.height);
  return true;
}

void CDoviEnhancementDecoder::Send(const std::uint8_t* data,
                                   int size,
                                   std::int64_t pts,
                                   std::int64_t dts)
{
  AVPacket* packet = av_packet_alloc();
  if (!packet || av_new_packet(packet, size) < 0)
  {
    av_packet_free(&packet);
    return;
  }
  std::memcpy(packet->data, data, size);
  packet->pts = pts;
  packet->dts = dts;
  if (av_bsf_send_packet(m_filter, packet) == 0)
  {
    while (av_bsf_receive_packet(m_filter, packet) == 0)
    {
      if (packet->size > 0 && avcodec_send_packet(m_decoder, packet) == AVERROR(EAGAIN))
      {
        Receive();
        avcodec_send_packet(m_decoder, packet);
      }
      av_packet_unref(packet);
      Receive();
    }
  }
  av_packet_free(&packet);
}

void CDoviEnhancementDecoder::Receive()
{
  AVFrame* frame = av_frame_alloc();
  while (frame && avcodec_receive_frame(m_decoder, frame) == 0)
  {
    VideoPicture picture;
    if (m_hardware && m_hardware->Decode(m_decoder, frame) == CDVDVideoCodec::VC_PICTURE &&
        m_hardware->GetPicture(m_decoder, &picture) && picture.videoBuffer)
    {
      m_frames.emplace_back(frame->pts,
                            std::shared_ptr<CVideoBuffer>(picture.videoBuffer,
                                                          [](CVideoBuffer* b) { b->Release(); }));
      picture.videoBuffer = nullptr;
      if (m_frames.size() > MAXIMUM_QUEUED)
        m_frames.pop_front();
    }
    av_frame_unref(frame);
  }
  av_frame_free(&frame);
}

std::shared_ptr<CVideoBuffer> CDoviEnhancementDecoder::Take(std::int64_t pts)
{
  while (!m_frames.empty() && m_frames.front().first < pts)
    m_frames.pop_front();
  if (m_frames.empty() || m_frames.front().first != pts)
    return {};
  std::shared_ptr<CVideoBuffer> picture = std::move(m_frames.front().second);
  m_frames.pop_front();
  return picture;
}

void CDoviEnhancementDecoder::Flush()
{
  m_frames.clear();
  av_bsf_flush(m_filter);
  avcodec_flush_buffers(m_decoder);
  if (m_hardware)
    m_hardware->Reset();
}
