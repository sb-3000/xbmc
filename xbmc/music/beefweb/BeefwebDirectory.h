/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BeefwebClient.h"
#include "filesystem/IDirectory.h"

#include <cstdint>
#include <string>

class CFileItemList;

namespace KODI::MUSIC::BEEFWEB
{

/*!
 * \brief Browses the media library of a remote Beefweb-controlled player.
 *
 * Paths are of the form
 * \verbatim
 *   beefweb://library/                     the top of the library
 *   beefweb://library/<folder>/.../        a folder within it
 * \endverbatim
 * following the folders of the library path the remote player supplied, each
 * one URL encoded. Listings are answered by the remote player, so the
 * arrangement seen here is its library's rather than Kodi's, and the folders
 * are relative to its music directories rather than to any disk.
 */
class CBeefwebDirectory : public XFILE::IDirectory
{
public:
  CBeefwebDirectory() = default;
  ~CBeefwebDirectory() override = default;

  bool GetDirectory(const CURL& url, CFileItemList& items) override;
  bool Exists(const CURL& url) override;
  XFILE::DIR_CACHE_TYPE GetCacheType(const CURL& url) const override
  {
    return XFILE::DIR_CACHE_NEVER;
  }

  /*! \brief Build a path for the library root. */
  static std::string RootPath();

private:
  /*!
   \brief Add a folder to a listing
   \param node the folder to add
   \param artworkUrl where the remote player serves its cover art
   \param items the list to add to
   */
  static void AddFolder(const BeefwebLibraryNode& node,
                        const std::string& artworkUrl,
                        CFileItemList& items);
  /*!
   \brief Add a track to a listing
   \param node the track to add
   \param folderPath path of the listing it belongs to, recorded so that Kodi
          builds the play queue from this listing rather than from the folder
          the track's file sits in
   \param startOffset where the track begins within its file, in milliseconds,
          which is how Kodi tells apart tracks that share one file
   \param artworkUrl where the remote player serves its cover art
   \param items the list to add to
   */
  static void AddTrack(const BeefwebLibraryNode& node,
                       const std::string& folderPath,
                       int64_t startOffset,
                       const std::string& artworkUrl,
                       CFileItemList& items);
};

} // namespace KODI::MUSIC::BEEFWEB
