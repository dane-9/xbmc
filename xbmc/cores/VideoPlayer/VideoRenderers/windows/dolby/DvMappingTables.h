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
#include <cstdint>
#include <optional>

namespace DVTUNNEL
{

constexpr unsigned BL_CODES = 1024;
constexpr unsigned MMR_FEATURES = 21;

/*!
 * \brief One component's reshaping curve and NLQ parameters.
 *
 * Coefficients are in FFmpeg's integer form: a value c stands for c / 2^denom.
 */
struct DvCurve
{
  bool mmr = false;
  unsigned numPivots = 0;
  std::uint16_t pivots[9] = {};
  unsigned polyOrder[8] = {};
  std::int64_t poly[8][3] = {};
  unsigned mmrOrder = 0;
  std::int64_t mmrConstant = 0;
  std::int64_t mmrCoef[MMR_FEATURES] = {};
  std::uint64_t nlqOffset = 0;
  std::int64_t slope = 0;
  std::int64_t maximum = 0;
  std::int64_t threshold = 0;
};

/*!
 * \brief The reshaping of all three components of a picture.
 */
struct DvMapping
{
  unsigned denom = 0;
  bool residual = false;
  DvCurve curves[3];
};

/*!
 * \brief Prediction and residual tables the GPU composer reads.
 */
struct DvTables
{
  unsigned denom = 0;
  bool residual = false;
  bool mmr[3] = {};
  // Prediction in Q16 (12-bit units x 16) per 10-bit code, polynomial
  // components only.
  std::array<std::int32_t, BL_CODES> predict[3] = {};
  // Signed residual in Q16 per 10-bit EL code; all zero without residual.
  std::array<std::int32_t, BL_CODES> residualQ16[3] = {};
  std::uint16_t pivotLow[3] = {};
  std::uint16_t pivotHigh[3] = {};
  unsigned mmrOrder[3] = {};
  std::int64_t mmrConstant[3] = {};
  std::int64_t mmrCoef[3][MMR_FEATURES] = {};

  bool AnyChromaMmr() const { return mmr[1] || mmr[2]; }
};

/*!
 * \brief The 12-bit output per 10-bit code of each component.
 */
using DvReshapeLut = std::array<std::array<std::uint16_t, BL_CODES>, 3>;

/*!
 * \brief Reads a single-layer polynomial mapping ("reshape mode").
 *
 * \param blob an AVDOVIMetadata side-data blob
 * \param size size of blob in bytes
 * \return the mapping with residual off and zero NLQ parameters, or empty if
 *         a component is not polynomial, a residual is present, or the blob is
 *         unreadable
 */
std::optional<DvMapping> ReadSingleLayerMapping(const void* blob, std::size_t size);

/*!
 * \brief Reads any mapping the composer handles ("compose mode"): MMR chroma,
 * a residual, or both.
 *
 * \return the mapping, or empty if the blob is unreadable or uses a method
 *         the composer does not implement
 */
std::optional<DvMapping> ReadComposerMapping(const void* blob, std::size_t size);

/*!
 * \brief Builds the prediction and residual tables of a mapping.
 *
 * The tables are written in place because the tunnel reuses them per picture.
 *
 * \return false when a coefficient is outside the range the composer computes
 *         exactly
 */
bool BuildTables(const DvMapping& mapping, DvTables& out);

/*!
 * \brief Builds the reshape lookup table from polynomial tables.
 *
 * \return false when a component is MMR
 */
bool BuildReshapeLut(const DvTables& tables, DvReshapeLut& lut);

/*!
 * \brief The MMR feature products of a pixel's three 10-bit codes, in the
 * coefficient order of the mapping.
 */
void MmrFeatures(std::uint32_t x0,
                 std::uint32_t x1,
                 std::uint32_t x2,
                 std::uint64_t (&f)[MMR_FEATURES]);

enum class DvVerdict
{
  ACCEPTED,
  UNSUPPORTED_PROFILE,
  UNREADABLE,
  RESIDUAL_OUTSIDE_PROFILE7,
  OUT_OF_RANGE,
};

/*!
 * \brief True for profiles 5, 8.1, 8.6 and 7 with compatibility ID 6 (UHD
 * Blu-ray).
 */
bool IsSupportedProfile(int profile, int compatibilityId);

/*!
 * \brief Whether the tunnel accepts a picture's metadata, and why not.
 */
DvVerdict ClassifyMetadata(const void* blob, std::size_t size, int profile, int compatibilityId);

} // namespace DVTUNNEL
