/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Horizontal 2x upsampling of an enhancement-layer plane, shared by the
// composer shaders.

int ElTap(Texture2D<uint> v, int c, int y, int w)
{
  return (int)v.Load(int3(clamp(c, 0, w - 1), y, 0));
}

uint ElCode(Texture2D<uint> v, int x, int y, int w)
{
  int c = x >> 1;
  int s = ElTap(v, c, y, w);
  if (x & 1)
    s = (22 * (ElTap(v, c - 3, y, w) + ElTap(v, c + 4, y, w)) +
         94 * (ElTap(v, c - 2, y, w) + ElTap(v, c + 3, y, w)) -
         524 * (ElTap(v, c - 1, y, w) + ElTap(v, c + 2, y, w)) +
         2456 * (s + ElTap(v, c + 1, y, w)) + 2048) >> 12;
  return (uint)clamp(s, 0, 1023);
}
