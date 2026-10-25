/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DvTunnelStats.h"

namespace DVTUNNEL
{

const char* SkipReasonText(DvSkip reason)
{
  switch (reason)
  {
    case DvSkip::UNSUPPORTED_PROFILE:
      return "unsupported profile";
    case DvSkip::METADATA_UNREADABLE:
      return "metadata missing or unreadable";
    case DvSkip::ENHANCEMENT_OUTSIDE_PROFILE7:
      return "enhancement layer outside Profile 7";
    case DvSkip::COEFFICIENTS_OUT_OF_RANGE:
      return "composer coefficients out of range";
    case DvSkip::PICTURE_UNAVAILABLE:
      return "decoded picture unavailable";
    case DvSkip::UNEXPECTED_PICTURE:
      return "unexpected picture size or format";
    case DvSkip::TRANSPORT_REJECTED:
      return "transport metadata rejected";
    case DvSkip::PACKETIZATION_FAILED:
      return "metadata packetization failed";
    case DvSkip::GPU_PACKING_FAILED:
      return "GPU packing failed";
    default:
      return "unknown";
  }
}

void CDvTunnelStats::Skip(DvSkip reason)
{
  ++m_skipped;
  bool& logged = m_skipLogged[static_cast<std::size_t>(reason)];
  if (!logged)
    m_log(DvLogLevel::WARNING, std::string("Dolby Vision tunnel: frame not packed (") +
                                   SkipReasonText(reason) + "); the previous frame is repeated.");
  logged = true;
}

void CDvTunnelStats::Packed()
{
  ++m_packed;
}

void CDvTunnelStats::Mode(const DvComposeMode& mode)
{
  if (m_haveMode)
    return;
  m_haveMode = true;
  if (mode.reshape)
    m_log(DvLogLevel::DEBUG, "Dolby Vision tunnel: reshaping the base layer alone.");
  else
    m_log(DvLogLevel::DEBUG,
          std::string("Dolby Vision tunnel: composing ") +
              (mode.enhancement ? "base and enhancement layers" : "the base layer") + " with " +
              (mode.mmrChroma ? "MMR" : "polynomial") + " chroma.");
}

void CDvTunnelStats::EnhancementMiss()
{
  if (!m_missLogged)
    m_log(DvLogLevel::WARNING, "Dolby Vision tunnel: composing without the enhancement layer.");
  m_missLogged = true;
}

std::string CDvTunnelStats::Summary() const
{
  return "Dolby Vision tunnel summary: " + std::to_string(m_packed) + " renders packed, " +
         std::to_string(m_skipped) + " not packed.";
}

void CDvTunnelStats::Finish()
{
  if (m_packed > 0 || m_skipped > 0)
    m_log(DvLogLevel::INFO, Summary());
  Logger log = std::move(m_log);
  *this = CDvTunnelStats(std::move(log));
}

} // namespace DVTUNNEL
