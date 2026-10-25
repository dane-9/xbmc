/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

extern "C"
{
#include <libavutil/attributes.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/version.h>
}

namespace DVTUNNEL
{

/*!
 * \brief Bounds-checked view of an AVDOVIMetadata side-data blob.
 *
 * The blob comes from the decoder and is treated as untrusted: every read
 * checks the range and returns a copy.
 */
class CDvBlob
{
public:
  CDvBlob(const void* data, std::size_t size)
    : m_data(static_cast<const std::uint8_t*>(data)),
      m_size(data ? size : 0)
  {
    m_valid = m_data && m_size >= sizeof(AVDOVIMetadata);
    if (m_valid)
      std::memcpy(&m_root, m_data, sizeof(m_root));
  }

  std::size_t Size() const { return m_size; }
  const AVDOVIMetadata& Root() const { return m_root; }

  /*!
   * \brief True if [offset, offset + length) lies inside the blob and after
   * the root.
   */
  bool RangeOk(std::size_t offset, std::size_t length) const
  {
    return m_valid && offset >= sizeof(AVDOVIMetadata) && offset <= m_size &&
           length <= m_size - offset;
  }

  /*!
   * \brief Reads a T at offset, or empty if it does not lie inside the blob.
   */
  template<typename T>
  std::optional<T> Read(std::size_t offset) const
  {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!RangeOk(offset, sizeof(T)))
      return {};
    T out;
    std::memcpy(&out, m_data + offset, sizeof(T));
    return out;
  }

  std::optional<AVDOVIRpuDataHeader> Header() const
  {
    return Read<AVDOVIRpuDataHeader>(m_root.header_offset);
  }
  std::optional<AVDOVIDataMapping> Mapping() const
  {
    return Read<AVDOVIDataMapping>(m_root.mapping_offset);
  }
  std::optional<AVDOVIColorMetadata> Color() const
  {
    return Read<AVDOVIColorMetadata>(m_root.color_offset);
  }

private:
  const std::uint8_t* m_data;
  std::size_t m_size;
  bool m_valid = false;
  AVDOVIMetadata m_root{};
};

} // namespace DVTUNNEL
