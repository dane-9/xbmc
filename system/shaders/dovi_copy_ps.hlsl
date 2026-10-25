/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Writes packed texels as exact UNORM8 bytes.

Texture2D<uint> gPacked : register(t0);

float4 main(float4 pos : SV_Position) : SV_Target
{
  uint v = gPacked.Load(int3(pos.xy, 0));
  return float4((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24) / 255.0;
}
