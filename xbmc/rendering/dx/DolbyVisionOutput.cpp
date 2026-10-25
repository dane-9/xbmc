/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DolbyVisionOutput.h"

#include "DeviceResources.h"
#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "messaging/ApplicationMessenger.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"

#include "platform/win32/CharsetConverter.h"
#include "platform/win32/WIN32Util.h"

#ifdef HAVE_NVAPI
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nvapi.h>
#include <windows.h>
#include <wrl/client.h>
#endif

#ifdef HAVE_NVAPI
namespace
{
// The driver's timings for the Dolby Vision output modes. It reports 24 Hz
// with the nominal 23.976 Hz rrx1k and 59.94 Hz with 60 Hz.
struct HdmiVicFormat
{
  NvU16 width;
  NvU16 height;
  NvU16 hFrontPorch;
  NvU16 hSyncWidth;
  NvU16 hTotal;
  NvU16 vFrontPorch;
  NvU16 vSyncWidth;
  NvU16 vTotal;
  NvU32 pclk;
  NvU32 rrx1k;
  unsigned numerator;
  unsigned denominator;
};

constexpr HdmiVicFormat HDMI_VIC_FORMATS[] = {
    {3840, 2160, 1276, 88, 5500, 8, 10, 2250, 29670, 23976, 24000, 1001},
    {3840, 2160, 1276, 88, 5500, 8, 10, 2250, 29700, 23976, 24, 1},
    {3840, 2160, 1056, 88, 5280, 8, 10, 2250, 29700, 25000, 25, 1},
    {3840, 2160, 176, 88, 4400, 8, 10, 2250, 29670, 29970, 30000, 1001},
    {3840, 2160, 1056, 88, 5280, 8, 10, 2250, 59400, 50000, 50, 1},
    {3840, 2160, 176, 88, 4400, 8, 10, 2250, 59341, 60000, 60000, 1001},
    {3840, 2160, 176, 88, 4400, 8, 10, 2250, 59400, 60000, 60, 1},
    {1920, 1080, 638, 44, 2750, 4, 5, 1125, 7418, 23976, 24000, 1001},
    {1920, 1080, 638, 44, 2750, 4, 5, 1125, 7425, 23976, 24, 1},
    {1920, 1080, 528, 44, 2640, 4, 5, 1125, 7425, 25000, 25, 1},
    {1920, 1080, 88, 44, 2200, 4, 5, 1125, 7418, 29970, 30000, 1001},
    {1920, 1080, 528, 44, 2640, 4, 5, 1125, 14850, 50000, 50, 1},
    {1920, 1080, 88, 44, 2200, 4, 5, 1125, 14835, 60000, 60000, 1001},
    {1920, 1080, 88, 44, 2200, 4, 5, 1125, 14850, 60000, 60, 1},
};

// Low 16 bits of the timing status: subtype 0x1b (the driver's HDMI_VIC timing)
// with HDMI_VIC 0. Current drivers only put the HDMI OUI in the mode-7 vendor
// InfoFrame for an HDMI_VIC timing; with this tag they send the classic Dolby
// Vision InfoFrame (format 0x20, HDMI_VIC byte 0xfe), which names no mode.
// Reverting the timing ends Dolby Vision output. The value is inferred from
// the driver, not documented by NVIDIA.
constexpr NvU32 DOLBY_VISION_TIMING_STATUS = 0x1b00;
} // namespace
#endif

struct DX::CDolbyVisionOutput::Impl
{
  HWND window{};
  bool candidate{};
  bool enableAttempted{};
  bool carrierSwapChain{};
  bool suspended{};
  bool pausedForFocus{};
  bool loggedGui{};
  double fps{};
  int width{};
  int height{};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> packedFrame;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> guiFrame;
  std::function<bool(ID3D11Texture2D*, ID3D11Texture2D*)> compositeGui;

#ifdef HAVE_NVAPI
  NvU32 displayId{};
  NV_HDR_COLOR_DATA originalHdr{};
  bool initialized{};
  bool hdmiVicTiming{};
#endif
  bool active{};
};

namespace
{
#ifdef HAVE_NVAPI
const HdmiVicFormat* FormatFor(long width, long height, double refresh)
{
  for (const HdmiVicFormat& format : HDMI_VIC_FORMATS)
  {
    if (format.width == width && format.height == height &&
        std::abs(refresh - static_cast<double>(format.numerator) / format.denominator) < 0.01)
      return &format;
  }
  return nullptr;
}

struct ActiveDisplay
{
  const HdmiVicFormat* format{nullptr};
  double refresh{0.0};
  bool hdmi{false};
};

// The output of Kodi's display, identified by its GDI source name.
ActiveDisplay GetActiveDisplay(const std::wstring& gdiName)
{
  ActiveDisplay display;
  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
  if (status != ERROR_SUCCESS || pathCount == 0)
    return display;

  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                              modes.data(), nullptr);
  if (status != ERROR_SUCCESS)
    return display;

  for (UINT32 i = 0; i < pathCount; ++i)
  {
    const DISPLAYCONFIG_PATH_INFO& path = paths[i];
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                     path.sourceInfo.adapterId, path.sourceInfo.id};
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        _wcsicmp(source.viewGdiDeviceName, gdiName.c_str()) != 0)
      continue;

    display.hdmi = path.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    if (path.targetInfo.modeInfoIdx >= modeCount ||
        modes[path.targetInfo.modeInfoIdx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
      return display;

    const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal =
        modes[path.targetInfo.modeInfoIdx].targetMode.targetVideoSignalInfo;
    display.refresh = path.targetInfo.refreshRate.Denominator == 0
                          ? 0.0
                          : static_cast<double>(path.targetInfo.refreshRate.Numerator) /
                                path.targetInfo.refreshRate.Denominator;
    display.format = FormatFor(signal.activeSize.cx, signal.activeSize.cy, display.refresh);
    return display;
  }
  return display;
}

// The tunnel is TV-led ("standard") Dolby Vision. Version 0 and 1 Dolby VSVDBs
// always include it; version 2 sinks state it in the interface field, where 0
// and 1 mean low-latency (player-led) only.
std::optional<NvU32> FindStandardDolbyVisionDisplay(const std::wstring& gdiName, bool log = true)
{
  NvU32 displayId = 0;
  std::string name; // GDI display names are ASCII (\\.\DISPLAYn)
  for (wchar_t c : gdiName)
    name += static_cast<char>(c);
  const NvAPI_Status idStatus = NvAPI_DISP_GetDisplayIdByDisplayName(name.c_str(), &displayId);
  if (idStatus != NVAPI_OK)
  {
    if (log)
      CLog::Log(LOGWARNING, "Dolby Vision output: {} is not an NVIDIA display (status {}).", name,
                static_cast<int>(idStatus));
    return {};
  }

  NV_HDR_CAPABILITIES capabilities{};
  capabilities.version = NV_HDR_CAPABILITIES_VER;
  const NvAPI_Status hdrStatus = NvAPI_Disp_GetHdrCapabilities(displayId, &capabilities);
  if (hdrStatus != NVAPI_OK || !capabilities.isDolbyVisionSupported)
  {
    if (log)
      CLog::Log(LOGWARNING,
                "Dolby Vision output: {} does not report Dolby Vision support (status {}, "
                "flag {}).",
                name, static_cast<int>(hdrStatus),
                static_cast<unsigned>(capabilities.isDolbyVisionSupported));
    return {};
  }
  const auto& dv = capabilities.dv_static_metadata;
  const bool standard = dv.VSVDB_version < 2 || dv.interface_supported_by_sink >= 2;
  if (log)
    CLog::Log(standard ? LOGDEBUG : LOGWARNING,
              "Dolby Vision output: {} reports Dolby VSVDB version {}, interface {}{}.", name,
              static_cast<unsigned>(dv.VSVDB_version),
              static_cast<unsigned>(dv.interface_supported_by_sink),
              standard ? "" : "; only low-latency Dolby Vision, which the tunnel does not send");
  if (!standard)
    return {};
  return displayId;
}

// Initializes NVAPI and finds Kodi's display; on success NVAPI stays
// initialized for the caller.
std::optional<NvU32> OpenDisplay(const std::wstring& gdiName, bool log = true)
{
  const NvAPI_Status init = NvAPI_Initialize();
  if (init != NVAPI_OK)
  {
    if (log)
      CLog::Log(LOGWARNING, "Dolby Vision output: NVAPI initialization failed (status {}).",
                static_cast<int>(init));
    return {};
  }
  const std::optional<NvU32> displayId = FindStandardDolbyVisionDisplay(gdiName, log);
  if (!displayId)
    NvAPI_Unload();
  return displayId;
}

std::optional<NV_HDR_COLOR_DATA> GetHdr(NvU32 displayId)
{
  NV_HDR_COLOR_DATA hdr{};
  hdr.version = NV_HDR_COLOR_DATA_VER;
  hdr.cmd = NV_HDR_CMD_GET;
  if (NvAPI_Disp_HdrColorControl(displayId, &hdr) != NVAPI_OK)
    return {};
  return hdr;
}

NvAPI_Status SetHdrStatus(NvU32 displayId, NV_HDR_COLOR_DATA hdr)
{
  hdr.version = NV_HDR_COLOR_DATA_VER;
  hdr.cmd = NV_HDR_CMD_SET;
  return NvAPI_Disp_HdrColorControl(displayId, &hdr);
}

bool ReportedSdr(NvU32 displayId)
{
  NV_DISPLAY_OUTPUT_MODE mode{};
  return NvAPI_Disp_GetOutputMode(displayId, &mode) == NVAPI_OK &&
         mode == NV_DISPLAY_OUTPUT_MODE_SDR;
}

std::optional<NV_TIMING> GetTiming(NvU32 displayId,
                                   NV_TIMING_OVERRIDE type,
                                   NvU32 width = 0,
                                   NvU32 height = 0,
                                   float refresh = 0.0f)
{
  NV_TIMING_INPUT input{};
  input.version = NV_TIMING_INPUT_VER;
  input.type = type;
  if (type != NV_TIMING_OVERRIDE_CURRENT)
  {
    input.width = width;
    input.height = height;
    input.rr = refresh;
  }
  NV_TIMING timing{};
  if (NvAPI_DISP_GetTiming(displayId, &input, &timing) != NVAPI_OK)
    return {};
  return timing;
}

bool IsMovieTiming(const NV_TIMING& t, const HdmiVicFormat& format)
{
  return t.HVisible == format.width && t.VVisible == format.height && t.HBorder == 0 &&
         t.VBorder == 0 && t.HTotal == format.hTotal && t.VTotal == format.vTotal &&
         t.pclk + 1 >= format.pclk && t.pclk <= format.pclk + 1 &&
         t.HFrontPorch == format.hFrontPorch && t.HSyncWidth == format.hSyncWidth &&
         t.VFrontPorch == format.vFrontPorch && t.VSyncWidth == format.vSyncWidth &&
         t.HSyncPol == 0 && t.VSyncPol == 0 && t.interlaced == 0 &&
         t.etc.rrx1k + 1 >= format.rrx1k && t.etc.rrx1k <= format.rrx1k + 1;
}

bool HasHdmiVicTiming(NvU32 displayId, const HdmiVicFormat& format)
{
  const std::optional<NV_TIMING> current = GetTiming(displayId, NV_TIMING_OVERRIDE_CURRENT);
  const bool tagged = current && IsMovieTiming(*current, format) &&
                      (current->etc.status & 0xffff) == DOLBY_VISION_TIMING_STATUS;
  if (!tagged)
    CLog::Log(LOGERROR, "Dolby Vision output: the HDMI_VIC 0 timing is missing (status {:#x}).",
              current ? current->etc.status : 0);
  return tagged;
}

bool ApplyHdmiVicTiming(NvU32 displayId, const HdmiVicFormat& format)
{
  // The EDID lookup can return the 1000/1001 variant; the current mode is the
  // one Kodi switched to.
  std::optional<NV_TIMING> timing =
      GetTiming(displayId, NV_TIMING_OVERRIDE_EDID, format.width, format.height,
                static_cast<float>(format.numerator) / format.denominator);
  if (!timing || !IsMovieTiming(*timing, format))
    timing = GetTiming(displayId, NV_TIMING_OVERRIDE_CURRENT);
  if (!timing || !IsMovieTiming(*timing, format))
  {
    CLog::Log(
        LOGERROR, "Dolby Vision output: the EDID {}x{}/{:.3f} timing is not the expected one.",
        format.width, format.height, static_cast<double>(format.numerator) / format.denominator);
    return false;
  }
  timing->etc.status = (timing->etc.status & 0xffff0000u) | DOLBY_VISION_TIMING_STATUS;
  NV_CUSTOM_DISPLAY custom{};
  custom.version = NV_CUSTOM_DISPLAY_VER;
  custom.width = format.width;
  custom.height = format.height;
  custom.depth = 32;
  custom.colorFormat = NV_FORMAT_A8R8G8B8;
  custom.srcPartition = {0.0f, 0.0f, 1.0f, 1.0f};
  custom.xRatio = 1.0f;
  custom.yRatio = 1.0f;
  custom.timing = *timing;
  // A trial timing is never saved to the display's mode list.
  NvU32 id = displayId;
  const NvAPI_Status status = NvAPI_DISP_TryCustomDisplay(&id, 1, &custom);
  if (status != NVAPI_OK)
  {
    CLog::Log(LOGERROR, "Dolby Vision output: HDMI_VIC 0 custom timing rejected ({}).",
              static_cast<int>(status));
    return false;
  }
  // The driver applies the timing with a mode set.
  for (unsigned attempt = 0; attempt < 20; ++attempt)
  {
    const std::optional<NV_TIMING> current = GetTiming(displayId, NV_TIMING_OVERRIDE_CURRENT);
    if (current && (current->etc.status & 0xffff) == DOLBY_VISION_TIMING_STATUS)
      return true;
    Sleep(100);
  }
  CLog::Log(LOGERROR, "Dolby Vision output: the HDMI_VIC 0 timing was not applied.");
  return false;
}

void RevertHdmiVicTiming(NvU32 displayId)
{
  NvU32 id = displayId;
  const NvAPI_Status status = NvAPI_DISP_RevertCustomDisplayTrial(&id, 1);
  if (status != NVAPI_OK)
    CLog::Log(LOGWARNING, "Dolby Vision output: reverting the HDMI_VIC timing failed ({}).",
              static_cast<int>(status));
}

struct EntryState
{
  const HdmiVicFormat* format{};
  NvU32 displayId{};
  NV_HDR_COLOR_DATA hdr{};
  NV_TIMING timing{};
  bool retainedReplay{};
};

// The read-only conditions for entering Dolby Vision output for a width x height
// stream at fps. On success NVAPI stays initialized for the caller.
std::optional<EntryState> CheckEntry(const std::wstring& gdiName, int width, int height, double fps)
{
  if (!DX::CDolbyVisionOutput::IsAllowedBySettings() ||
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
          CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) == ADJUST_REFRESHRATE_OFF ||
      fps <= 0.0)
    return {};

  EntryState entry;
  const DX::CDolbyVisionOutput::OutputMode mode =
      DX::CDolbyVisionOutput::SelectedMode(width, height, fps);
  entry.format = FormatFor(mode.width, mode.height, mode.refresh);
  if (!entry.format)
  {
    CLog::Log(LOGWARNING,
              "Dolby Vision output: no supported display mode for a {}x{} stream ({}x{} {:.3f} Hz "
              "selected).",
              width, height, mode.width, mode.height, mode.refresh);
    return {};
  }

  const ActiveDisplay current = GetActiveDisplay(gdiName);
  if (!current.hdmi)
  {
    CLog::Log(LOGWARNING, "Dolby Vision output: requires an HDMI connection.");
    return {};
  }

  const std::optional<NvU32> displayId = OpenDisplay(gdiName);
  if (!displayId)
    return {};
  entry.displayId = *displayId;
  const std::optional<NV_HDR_COLOR_DATA> hdr = GetHdr(entry.displayId);
  const std::optional<NV_TIMING> timing = GetTiming(entry.displayId, NV_TIMING_OVERRIDE_CURRENT);
  if (!hdr || !timing)
  {
    NvAPI_Unload();
    return {};
  }
  entry.hdr = *hdr;
  entry.timing = *timing;

  const NV_HDR_MODE entryMode = entry.hdr.hdrMode;
  // The driver keeps reporting mode 7 after a completed exit. That is accepted on
  // an SDR desktop whose current timing is an untagged movie timing; it can still
  // be the desktop rate while Kodi switches to the stream's.
  entry.retainedReplay = entryMode == NV_HDR_MODE_DOLBY_VISION && ReportedSdr(entry.displayId) &&
                         current.format && IsMovieTiming(entry.timing, *current.format) &&
                         (entry.timing.etc.status & 0xffff) != DOLBY_VISION_TIMING_STATUS;
  if ((entryMode != NV_HDR_MODE_OFF && !entry.retainedReplay) ||
      CWIN32Util::GetWindowsHDRStatus() != HDR_STATUS::HDR_OFF)
  {
    CLog::Log(LOGWARNING, "Dolby Vision output: requires an SDR desktop with Windows HDR off.");
    NvAPI_Unload();
    return {};
  }
  return entry;
}
#endif
} // namespace

DX::CDolbyVisionOutput::CDolbyVisionOutput() : m_impl(std::make_unique<Impl>())
{
}

DX::CDolbyVisionOutput::~CDolbyVisionOutput()
{
  Restore();
}

bool DX::CDolbyVisionOutput::IsAllowedBySettings()
{
  return CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
      CSettings::SETTING_VIDEOPLAYER_DOLBYVISIONOUTPUT);
}

DX::CDolbyVisionOutput::OutputMode DX::CDolbyVisionOutput::SelectedMode(int width,
                                                                        int height,
                                                                        double fps)
{
  const RESOLUTION_INFO target = CDisplaySettings::GetInstance().GetResolutionInfo(
      CResolutionUtils::ChooseBestResolution(static_cast<float>(fps), width, height, false));
  return {static_cast<unsigned>(target.iScreenWidth), static_cast<unsigned>(target.iScreenHeight),
          target.fRefreshRate};
}

bool DX::CDolbyVisionOutput::IsSupportedDisplay(const std::wstring& gdiName)
{
#ifdef HAVE_NVAPI
  auto check = [&gdiName](bool log)
  {
    if (!GetActiveDisplay(gdiName).hdmi)
    {
      if (log)
        CLog::Log(LOGWARNING, "Dolby Vision output: {} is not an active HDMI output.",
                  KODI::PLATFORM::WINDOWS::FromW(gdiName));
      return false;
    }
    if (!OpenDisplay(gdiName, log))
      return false;
    NvAPI_Unload();
    return true;
  };

  // Capability queries are frequent (settings, info labels), so the answer is
  // kept for a while and its reason is logged only when it changes.
  static std::mutex cacheLock;
  static std::wstring cachedName;
  static int cachedResult = -1;
  static std::chrono::steady_clock::time_point checkedAt;
  std::unique_lock lock(cacheLock);
  const auto now = std::chrono::steady_clock::now();
  if (gdiName == cachedName && cachedResult >= 0 && now - checkedAt < std::chrono::seconds(10))
    return cachedResult != 0;

  const bool supported = check(false);
  if (gdiName != cachedName || static_cast<int>(supported) != cachedResult)
    check(true);
  cachedName = gdiName;
  cachedResult = supported;
  checkedAt = now;
  return supported;
#else
  return false;
#endif
}

bool DX::CDolbyVisionOutput::CanEnableOn(const std::wstring& gdiName,
                                         int width,
                                         int height,
                                         double fps)
{
#ifdef HAVE_NVAPI
  if (!CheckEntry(gdiName, width, height, fps))
    return false;
  NvAPI_Unload();
  return true;
#else
  return false;
#endif
}

bool DX::CDolbyVisionOutput::Enable(const std::wstring& gdiName,
                                    int width,
                                    int height,
                                    double fps,
                                    const std::function<bool()>& prepareOutput)
{
#ifdef HAVE_NVAPI
  const std::optional<EntryState> entry = CheckEntry(gdiName, width, height, fps);
  if (!entry)
    return false;
  m_impl->initialized = true;
  m_impl->displayId = entry->displayId;
  m_impl->originalHdr = entry->hdr;
  const HdmiVicFormat& format = *entry->format;
  if (entry->retainedReplay)
    m_impl->originalHdr.hdrMode = NV_HDR_MODE_OFF;

  m_impl->hdmiVicTiming = true;
  if (!ApplyHdmiVicTiming(m_impl->displayId, format))
  {
    Restore();
    return false;
  }

  NV_HDR_COLOR_DATA dolbyVision = m_impl->originalHdr;
  dolbyVision.hdrMode = NV_HDR_MODE_DOLBY_VISION;
  dolbyVision.hdrColorFormat = NV_COLOR_FORMAT_RGB;
  dolbyVision.hdrDynamicRange = NV_DYNAMIC_RANGE_VESA;
  dolbyVision.hdrBpc = NV_BPC_8;

  // Windows lists the HDMI_VIC timing at the driver's nominal rate; the
  // timing checks below still require the exact pixel clock.
  unsigned numerator = format.numerator;
  unsigned denominator = format.denominator;
  if (std::abs(static_cast<double>(numerator) / denominator - format.rrx1k / 1000.0) >= 0.01)
  {
    numerator = format.rrx1k;
    denominator = 1000;
  }
  // Wait for Kodi's refresh-rate switch to reach the display.
  bool ready = false;
  double refresh = 0.0;
  if (prepareOutput())
  {
    for (unsigned attempt = 0; attempt < 20 && !ready; ++attempt)
    {
      const ActiveDisplay display = GetActiveDisplay(gdiName);
      refresh = display.refresh;
      ready =
          display.format && std::abs(refresh - static_cast<double>(numerator) / denominator) < 0.01;
      if (!ready)
        Sleep(100);
    }
  }
  if (!ready)
  {
    CLog::Log(LOGERROR, "Dolby Vision output: output preparation failed (refresh {:.3f}Hz).",
              refresh);
    Restore();
    return false;
  }

  const NvAPI_Status setStatus = SetHdrStatus(m_impl->displayId, dolbyVision);
  const std::optional<NV_HDR_COLOR_DATA> observed = GetHdr(m_impl->displayId);
  m_impl->active = true;
  if (setStatus != NVAPI_OK || !observed || observed->hdrMode != NV_HDR_MODE_DOLBY_VISION ||
      observed->hdrColorFormat != NV_COLOR_FORMAT_RGB ||
      observed->hdrDynamicRange != NV_DYNAMIC_RANGE_VESA || observed->hdrBpc != NV_BPC_8)
  {
    CLog::Log(
        LOGERROR,
        "Dolby Vision output: NVAPI rejected or did not verify mode 7 (set {}, get {}, mode {}).",
        static_cast<int>(setStatus), observed ? "ok" : "failed",
        observed ? static_cast<int>(observed->hdrMode) : -1);
    Restore();
    return false;
  }
  if (!HasHdmiVicTiming(m_impl->displayId, format))
  {
    Restore();
    return false;
  }
  CLog::Log(LOGINFO, "Dolby Vision output: entered at {}x{} {:.3f} Hz.", format.width,
            format.height, static_cast<double>(numerator) / denominator);
  return true;
#else
  CLog::Log(LOGWARNING, "Dolby Vision output: this build has no NVAPI support.");
  return false;
#endif
}

void DX::CDolbyVisionOutput::Restore()
{
#ifdef HAVE_NVAPI
  if (!m_impl->initialized)
    return;

  // Reverting the HDMI_VIC timing stops the Dolby Vision InfoFrame.
  if (m_impl->hdmiVicTiming)
    RevertHdmiVicTiming(m_impl->displayId);
  m_impl->hdmiVicTiming = false;
  if (m_impl->active)
  {
    const NvAPI_Status off = SetHdrStatus(m_impl->displayId, m_impl->originalHdr);
    if (off != NVAPI_OK)
      CLog::Log(LOGERROR, "Dolby Vision output: restoring the HDR mode failed ({}).",
                static_cast<int>(off));
    m_impl->active = false;
  }
  NvAPI_Unload();
  m_impl->initialized = false;
#endif
}

bool DX::CDolbyVisionOutput::IsActive() const
{
  return m_impl->active;
}

void DX::CDolbyVisionOutput::RemoveLeftoverTiming(const std::wstring& gdiName)
{
#ifdef HAVE_NVAPI
  // A Kodi that ended during Dolby Vision playback leaves the tagged trial timing.
  const std::optional<NvU32> display = OpenDisplay(gdiName, false);
  if (!display)
    return;
  const NvU32 displayId = *display;
  const std::optional<NV_TIMING> timing = GetTiming(displayId, NV_TIMING_OVERRIDE_CURRENT);
  if (timing && (timing->etc.status & 0xffff) == DOLBY_VISION_TIMING_STATUS)
  {
    CLog::Log(LOGWARNING, "Dolby Vision output: removing a Dolby Vision timing left by a previous "
                          "session.");
    RevertHdmiVicTiming(displayId);
    std::optional<NV_HDR_COLOR_DATA> hdr = GetHdr(displayId);
    if (hdr && hdr->hdrMode == NV_HDR_MODE_DOLBY_VISION)
    {
      hdr->hdrMode = NV_HDR_MODE_OFF;
      SetHdrStatus(displayId, *hdr);
    }
  }
  NvAPI_Unload();
#endif
}

std::wstring DX::CDolbyVisionOutput::DisplayName() const
{
  MONITORINFOEXW monitor{};
  monitor.cbSize = sizeof(monitor);
  if (!GetMonitorInfoW(MonitorFromWindow(m_impl->window, MONITOR_DEFAULTTONEAREST), &monitor))
    return {};
  return monitor.szDevice;
}

void DX::CDolbyVisionOutput::SetWindow(HWND window)
{
  m_impl->window = window;
  const std::wstring display = DisplayName();
  if (!display.empty())
    RemoveLeftoverTiming(display);
}

bool DX::CDolbyVisionOutput::CanEnable(int width, int height, double fps) const
{
  return IsActive() || CanEnableOn(DisplayName(), width, height, fps);
}

void DX::CDolbyVisionOutput::SetCandidate(bool candidate, double fps, int width, int height)
{
  Impl& d = *m_impl;
  if (candidate && !d.candidate)
  {
    d.enableAttempted = false;
    d.fps = fps;
    d.width = width;
    d.height = height;
    d.suspended = false;
    d.pausedForFocus = false;
    d.loggedGui = false;
  }
  d.candidate = candidate;
  // Renderer replacement can set a new candidate before the next present.
  if (!candidate)
    d.compositeGui = {};
}

bool DX::CDolbyVisionOutput::IsCandidate() const
{
  return m_impl->candidate;
}

void DX::CDolbyVisionOutput::SetFrame(
    ID3D11Texture2D* frame, std::function<bool(ID3D11Texture2D*, ID3D11Texture2D*)> compositeGUI)
{
  if (!m_impl->candidate || !frame)
    return;
  m_impl->packedFrame = frame;
  m_impl->compositeGui = std::move(compositeGUI);
}

bool DX::CDolbyVisionOutput::UsesCarrierSwapChain() const
{
  return m_impl->carrierSwapChain;
}

bool DX::CDolbyVisionOutput::PrepareCarrierSwapChain(DeviceResources& resources,
                                                     unsigned width,
                                                     unsigned height)
{
  m_impl->carrierSwapChain = true;
  resources.RecreateSwapChain();
  CD3DTexture& backBuffer = resources.GetBackBuffer();
  if (!resources.GetSwapChain() || !backBuffer.Get())
    return false;

  // Kodi's refresh-rate switching and the NVAPI timing set the display mode; the
  // borderless 8-bit swapchain only needs to cover it so Windows flips it unchanged.
  BOOL fullscreen = FALSE;
  resources.GetSwapChain()->GetFullscreenState(&fullscreen, nullptr);
  if (fullscreen || backBuffer.GetFormat() != DXGI_FORMAT_B8G8R8A8_UNORM ||
      backBuffer.GetWidth() != width || backBuffer.GetHeight() != height)
  {
    CLog::Log(
        LOGERROR,
        "Dolby Vision output: borderless {}x{} BGRA8 swapchain unavailable (exclusive {}, {}x{} "
        "format {}).",
        width, height, fullscreen != FALSE, backBuffer.GetWidth(), backBuffer.GetHeight(),
        static_cast<int>(backBuffer.GetFormat()));
    return false;
  }

  CLog::Log(LOGDEBUG, "Dolby Vision output: borderless {}x{} BGRA8 swapchain prepared.", width,
            height);
  return true;
}

void DX::CDolbyVisionOutput::Leave(DeviceResources* resources)
{
  Impl& d = *m_impl;
  d.compositeGui = {};
  d.guiFrame.Reset();
  d.packedFrame.Reset();
  Restore();
  if (d.carrierSwapChain)
  {
    d.carrierSwapChain = false;
    if (resources)
    {
      // Without the Dolby Vision signal the last carrier frame shows as false
      // colours; blank it while the display resynchronizes.
      CD3DTexture& backBuffer = resources->GetBackBuffer();
      if (backBuffer.Get() && resources->GetSwapChain())
      {
        constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        resources->GetImmediateContext()->ClearRenderTargetView(backBuffer.GetRenderTarget(),
                                                                black);
        resources->GetSwapChain()->Present(0, 0);
      }
      resources->RecreateSwapChain();
    }
  }
}

void DX::CDolbyVisionOutput::Reset()
{
  Leave(nullptr);
  m_impl->candidate = false;
}

bool DX::CDolbyVisionOutput::Present(DeviceResources& resources)
{
  Impl& d = *m_impl;
  const bool foreground = GetForegroundWindow() == d.window;
  if (d.suspended && foreground)
  {
    CLog::Log(LOGINFO, "Dolby Vision output: Kodi has the foreground again; re-entering.");
    d.suspended = false;
    d.enableAttempted = false;
  }

  if (d.candidate && d.packedFrame && !IsActive() && !d.enableAttempted && !d.suspended)
  {
    d.enableAttempted = true;
    D3D11_TEXTURE2D_DESC frameDesc{};
    d.packedFrame->GetDesc(&frameDesc);
    if (!Enable(DisplayName(), d.width, d.height, d.fps, [this, &resources, &frameDesc]
                { return PrepareCarrierSwapChain(resources, frameDesc.Width, frameDesc.Height); }))
    {
      CLog::Log(LOGWARNING,
                "Dolby Vision output: entry failed; falling back to Kodi's HDR renderer.");
      SetCandidate(false);
    }
    if (d.pausedForFocus)
    {
      d.pausedForFocus = false;
      CServiceBroker::GetAppMessenger()->PostMsg(TMSG_MEDIA_UNPAUSE);
    }
  }

  if (!d.candidate && (IsActive() || d.carrierSwapChain))
  {
    Leave(&resources);
    return false;
  }

  if (d.candidate && IsActive() && d.packedFrame)
  {
    if (!foreground)
    {
      Suspend(resources);
      return false;
    }
    PresentFrame(resources);
  }
  return true;
}

// Windows composes other windows into the carrier, which the TV shows as black,
// and without Dolby Vision processing a Profile 5 picture shows false colours. So
// playback pauses and the output is left until Kodi has the foreground again.
void DX::CDolbyVisionOutput::Suspend(DeviceResources& resources)
{
  Impl& d = *m_impl;
  DWORD process = 0;
  wchar_t windowClass[64]{};
  const HWND foreground = GetForegroundWindow();
  GetWindowThreadProcessId(foreground, &process);
  GetClassNameW(foreground, windowClass, 64);
  CLog::Log(LOGINFO,
            "Dolby Vision output: Kodi lost the foreground to process {} ({}); pausing and "
            "leaving the output.",
            process, KODI::PLATFORM::WINDOWS::FromW(windowClass));

  const auto& player = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  if (player->IsPlaying() && !player->IsPaused())
  {
    d.pausedForFocus = true;
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_MEDIA_PAUSE_IF_PLAYING);
  }

  // Re-entering needs a packed frame, and a paused renderer may not render again.
  const Microsoft::WRL::ComPtr<ID3D11Texture2D> frame = d.packedFrame;
  auto compositeGui = d.compositeGui;
  Leave(&resources);
  d.packedFrame = frame;
  d.compositeGui = std::move(compositeGui);
  d.suspended = true;
}

void DX::CDolbyVisionOutput::PresentFrame(DeviceResources& resources)
{
  Impl& d = *m_impl;
  CD3DTexture& backBuffer = resources.GetBackBuffer();
  D3D11_TEXTURE2D_DESC frameDesc{};
  d.packedFrame->GetDesc(&frameDesc);
  if (!backBuffer.Get() || frameDesc.Width != backBuffer.GetWidth() ||
      frameDesc.Height != backBuffer.GetHeight())
  {
    CLog::Log(LOGERROR,
              "Dolby Vision output: packed frame does not match the 8-bit swapchain dimensions.");
    d.candidate = false;
    return;
  }

  ID3D11DeviceContext* context = resources.GetImmediateContext();
  resources.GetD3DContext()->OMSetRenderTargets(0, nullptr, nullptr);
  context->OMSetRenderTargets(0, nullptr, nullptr);
  // Fullscreen video usually draws no GUI; compositing an empty layer would cost a
  // full-frame copy and pass on every present.
  bool guiReady = false;
  if (d.compositeGui && CServiceBroker::GetGUI()->GetWindowManager().HasVisibleControls())
  {
    if (!d.guiFrame)
    {
      D3D11_TEXTURE2D_DESC desc = frameDesc;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      desc.CPUAccessFlags = desc.MiscFlags = 0;
      resources.GetD3DDevice()->CreateTexture2D(&desc, nullptr, &d.guiFrame);
    }
    if (d.guiFrame)
    {
      context->CopyResource(d.guiFrame.Get(), backBuffer.Get());
      guiReady = true;
    }
  }
  context->CopyResource(backBuffer.Get(), d.packedFrame.Get());
  if (guiReady)
  {
    const bool composited = d.compositeGui(d.guiFrame.Get(), backBuffer.Get());
    if (!d.loggedGui)
    {
      CLog::Log(composited ? LOGDEBUG : LOGWARNING,
                "Dolby Vision output: source-domain GUI composition {}.",
                composited ? "active" : "failed");
      d.loggedGui = true;
    }
  }
}
