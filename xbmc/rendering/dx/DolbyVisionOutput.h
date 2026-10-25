/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <d3d11.h>

namespace DX
{
class DeviceResources;

/*!
 * \brief TV-led Dolby Vision output on NVIDIA HDMI displays.
 *
 * The renderer offers carrier frames packed by CDoviTunnel; DeviceResources
 * presents them through this class on the render thread. Entering applies an
 * HDMI_VIC 0 trial timing and the driver's Dolby Vision mode; leaving reverts
 * both.
 */
class CDolbyVisionOutput
{
public:
  struct Impl;

  CDolbyVisionOutput();
  ~CDolbyVisionOutput();

  CDolbyVisionOutput(const CDolbyVisionOutput&) = delete;
  CDolbyVisionOutput& operator=(const CDolbyVisionOutput&) = delete;

  /*!
   * \brief Sets the window Kodi presents to.
   *
   * Also reverts a Dolby Vision timing that a previous Kodi session left
   * applied on the window's display.
   */
  void SetWindow(HWND window);

  /*!
   * \brief Checks the read-only conditions for entering Dolby Vision output.
   *
   * Requires the setting, refresh-rate switching, a supported display mode and
   * display, and an SDR desktop. Called before playback renders, so a refusal
   * still lets the stream start as plain HDR. Dolby Vision is signalled at the
   * mode Kodi's refresh-rate switching selects for the stream, even before the
   * switch reaches the display.
   *
   * \param width picture width of the stream
   * \param height picture height of the stream
   * \param fps frame rate of the stream
   * \return true if Dolby Vision output can be entered
   */
  bool CanEnable(int width, int height, double fps) const;

  /*!
   * \brief Offers Dolby Vision output for a stream, or withdraws the offer.
   *
   * Output is entered on the next present after the first frame is set.
   */
  void SetCandidate(bool candidate, double fps = 0.0, int width = 0, int height = 0);
  bool IsCandidate() const;
  bool IsActive() const;

  /*!
   * \brief Sets the carrier frame to present next.
   *
   * \param frame the packed carrier frame
   * \param compositeGUI blends Kodi's GUI (first texture) into the presented
   *        frame (second texture); false when the GUI cannot be blended
   */
  void SetFrame(ID3D11Texture2D* frame,
                std::function<bool(ID3D11Texture2D*, ID3D11Texture2D*)> compositeGUI);

  /*!
   * \brief True while the output needs the 8-bit carrier swapchain instead of
   * Kodi's own.
   */
  bool UsesCarrierSwapChain() const;

  /*!
   * \brief Enters, maintains or leaves Dolby Vision output before a present.
   *
   * When Kodi loses the foreground, playback is paused and the output left;
   * both resume when Kodi has the foreground again.
   *
   * \return false when the caller must not present Kodi's back buffer
   */
  bool Present(DeviceResources& resources);

  /*!
   * \brief Leaves Dolby Vision output without recreating the swapchain, for
   * device loss and release.
   */
  void Reset();

  /*!
   * \brief Checks whether a display accepts standard (TV-led) Dolby Vision.
   *
   * \param gdiName the display's GDI device name
   * \return true for an NVIDIA HDMI display whose Dolby VSVDB allows TV-led
   *         Dolby Vision
   */
  static bool IsSupportedDisplay(const std::wstring& gdiName);

  /*!
   * \brief True when the "Send Dolby Vision to the display" setting is enabled.
   */
  static bool IsAllowedBySettings();

  struct OutputMode
  {
    unsigned width{0};
    unsigned height{0};
    double refresh{0.0};
  };

  /*!
   * \brief Returns the display mode Kodi's refresh-rate switching selects for a
   * stream.
   *
   * Dolby Vision is sent at this mode, and the picture is scaled to fit it.
   */
  static OutputMode SelectedMode(int width, int height, double fps);

private:
  static bool CanEnableOn(const std::wstring& gdiName, int width, int height, double fps);
  // gdiName is Kodi's display (\\.\DISPLAYn). prepareOutput readies the
  // presentation for the selected mode.
  bool Enable(const std::wstring& gdiName,
              int width,
              int height,
              double fps,
              const std::function<bool()>& prepareOutput);
  void Restore();
  static void RemoveLeftoverTiming(const std::wstring& gdiName);
  std::wstring DisplayName() const;
  bool PrepareCarrierSwapChain(DeviceResources& resources, unsigned width, unsigned height);
  // Leaves the output; with resources, the swapchain returns to Kodi's format.
  void Leave(DeviceResources* resources);
  void PresentFrame(DeviceResources& resources);
  // Leaves the output while Kodi is not in the foreground.
  void Suspend(DeviceResources& resources);

  std::unique_ptr<Impl> m_impl;
};
} // namespace DX
