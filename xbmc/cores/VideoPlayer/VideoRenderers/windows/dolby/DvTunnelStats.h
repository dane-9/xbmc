/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>

namespace DVTUNNEL
{

enum class DvSkip
{
  UNSUPPORTED_PROFILE,
  METADATA_UNREADABLE,
  ENHANCEMENT_OUTSIDE_PROFILE7,
  COEFFICIENTS_OUT_OF_RANGE,
  PICTURE_UNAVAILABLE,
  UNEXPECTED_PICTURE,
  TRANSPORT_REJECTED,
  PACKETIZATION_FAILED,
  GPU_PACKING_FAILED,
  COUNT
};

enum class DvLogLevel
{
  DEBUG,
  INFO,
  WARNING,
};

struct DvComposeMode
{
  bool reshape = true;
  bool enhancement = false; // the mapping's residual flag, before any fallback
  bool mmrChroma = false;
};

/*!
 * \brief Counts the tunnel's frames and writes its log lines.
 *
 * Logs the first frame skipped for each reason, the first composer mode, the
 * first frame composed without its enhancement layer, and a summary when
 * playback ends.
 */
class CDvTunnelStats
{
public:
  using Logger = std::function<void(DvLogLevel, const std::string&)>;

  explicit CDvTunnelStats(Logger logger) : m_log(std::move(logger)) {}

  void Skip(DvSkip reason);
  void Packed();
  void Mode(const DvComposeMode& mode);
  void EnhancementMiss();

  /*!
   * \brief Logs the summary if any frame was rendered, then clears all
   * counters.
   */
  void Finish();
  std::string Summary() const;

private:
  Logger m_log;
  std::array<bool, static_cast<std::size_t>(DvSkip::COUNT)> m_skipLogged{};
  std::size_t m_packed = 0;
  std::size_t m_skipped = 0;
  bool m_haveMode = false;
  bool m_missLogged = false;
};

const char* SkipReasonText(DvSkip reason);

} // namespace DVTUNNEL
