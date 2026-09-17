/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BeefwebDirectory.h"

#include "BeefwebClient.h"
#include "BeefwebPlayer.h"
#include "FileItem.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "guilib/LocalizeStrings.h"
#include "music/tags/MusicInfoTag.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/FileExtensionProvider.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

using namespace KODI::MUSIC::BEEFWEB;

namespace
{
constexpr auto LIBRARY_HOST = "library";

/*! \brief Separator between folders in a library path, on every platform. */
constexpr char LIBRARY_SEPARATOR = '/';

/*!
 * \brief Build the Kodi path addressing a node of the remote library.
 *
 * Each folder of the library path becomes a folder of the Kodi one, so that
 * going up a level in Kodi lands on the parent folder rather than back at the
 * top. Every element is encoded on its own, names being free to hold anything
 * a URL cannot.
 */
std::string PathForNode(const std::string& libraryPath)
{
  std::string path = CBeefwebDirectory::RootPath();

  for (const auto& element : StringUtils::Split(libraryPath, LIBRARY_SEPARATOR))
  {
    if (!element.empty())
      path += CURL::Encode(element) + "/";
  }

  return path;
}

/*! \brief Recover the library path from a Kodi path. */
std::string LibraryPathFromUrl(const CURL& url)
{
  std::vector<std::string> elements;

  for (const auto& element : StringUtils::Split(url.GetFileName(), '/'))
  {
    if (!element.empty())
      elements.emplace_back(CURL::Decode(element));
  }

  return StringUtils::Join(elements, std::string(1, LIBRARY_SEPARATOR));
}

/*!
 * \brief Position of a track on its album.
 *
 * Falls back to the number of the track within its file, which for an album
 * held in a single file amounts to the same thing and is there even when the
 * tags are not.
 */
int TrackNumberOf(const BeefwebTrack& track)
{
  const int number = std::atoi(track.trackNumber.c_str());

  return number > 0 ? number : track.subsong;
}

/*!
 * \brief Query restricting a listing to the area the user prefers.
 *
 * The remote player reports how many channels a track was mixed for, which is
 * what tells the areas of a disc apart. Mono counts as stereo here: it is not
 * a multichannel mix, and a mono recording should not disappear because a
 * preference was expressed about surround sound.
 *
 * \return an empty string when everything is to be shown.
 */
std::string AreaQuery()
{
  const int area = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      CSettings::SETTING_MUSICPLAYER_EXTERNALPLAYERAREA);

  switch (area)
  {
    case EXTERNAL_PLAYER_AREA_STEREO:
      return "%channels% IS stereo OR %channels% IS mono";
    case EXTERNAL_PLAYER_AREA_MULTICHANNEL:
      return "NOT %channels% IS stereo AND NOT %channels% IS mono";
    default:
      return {};
  }
}

/*!
 * \brief Where the remote player serves the cover art for a folder.
 *
 * An address rather than the image itself, so that Kodi fetches and caches it
 * as it would any other remote artwork. The picture kept beside the folder on
 * disk is asked for. Left to itself the remote player answers with the art of
 * the first track below instead, which for a folder holding many albums is
 * simply whichever of them comes first by name.
 */
std::string FolderArtworkUrl(const CBeefwebClient& client, const std::string& libraryPath)
{
  return client.GetBaseUrl() + "/artwork/library?path=" + CURL::Encode(libraryPath) +
         "&folderImage=true";
}

/*!
 * \brief Where the remote player serves the cover art for a track.
 *
 * The number within the file is always given, zero included, since that is
 * how a file holding a single track numbers it.
 */
std::string TrackArtworkUrl(const CBeefwebClient& client,
                            const std::string& libraryPath,
                            int subsong)
{
  return client.GetBaseUrl() + "/artwork/library?path=" + CURL::Encode(libraryPath) +
         "&subsong=" + std::to_string(subsong);
}
} // unnamed namespace

std::string CBeefwebDirectory::RootPath()
{
  return std::string("beefweb://") + LIBRARY_HOST + "/";
}

bool CBeefwebDirectory::Exists(const CURL& url)
{
  return url.IsProtocol("beefweb") && url.GetHostName() == LIBRARY_HOST;
}

bool CBeefwebDirectory::GetDirectory(const CURL& url, CFileItemList& items)
{
  if (url.GetHostName() != LIBRARY_HOST)
  {
    CLog::LogF(LOGERROR, "unknown location '{}'", url.GetRedacted());
    return false;
  }

  CBeefwebClient client;
  client.SetServerFromSettings();

  // Kodi has no notion of paging through a directory, so a whole level is
  // fetched at once.
  BeefwebFolderListing listing;
  if (!client.GetLibraryFolder(LibraryPathFromUrl(url), AreaQuery(), 0, -1, listing))
  {
    CLog::LogF(LOGERROR, "could not list '{}'", url.GetRedacted());
    return false;
  }

  bool hasFolders = false;
  bool hasTracks = false;

  // Where each track begins within its file. Tracks of an album held in one
  // file follow one another, so their offsets accumulate; a track that is a
  // file of its own starts at the beginning. Kodi tells items apart by path
  // and offset together, and without distinct offsets it would take every
  // track of such an album for the same one and keep only the first.
  std::string offsetPath;
  int64_t startOffset = 0;

  for (const auto& node : listing.nodes)
  {
    if (node.isFolder)
    {
      AddFolder(node, FolderArtworkUrl(client, node.path), items);
      hasFolders = true;
      continue;
    }

    if (node.track.path != offsetPath)
    {
      offsetPath = node.track.path;
      startOffset = 0;
    }

    AddTrack(node, url.Get(), startOffset,
             TrackArtworkUrl(client, node.path, node.track.subsong), items);
    hasTracks = true;

    // Advance by at least a moment even where the length is unknown, so that
    // two tracks can never end up claiming the same start.
    startOffset += std::max<int64_t>(1, static_cast<int64_t>(node.track.duration) * 1000);
  }

  // Levels holding both are labelled as songs, that being what the view has to
  // be able to show.
  if (hasTracks || !hasFolders)
    items.SetContent("songs");
  else
    items.SetContent("files");

  items.SetLabel(listing.path.empty() ? g_localizeStrings.Get(39210)
                                      : URIUtils::GetFileName(listing.path));

  return true;
}

void CBeefwebDirectory::AddFolder(const BeefwebLibraryNode& node,
                                  const std::string& artworkUrl,
                                  CFileItemList& items)
{
  auto item = std::make_shared<CFileItem>(node.name);
  item->SetPath(PathForNode(node.path));
  item->m_bIsFolder = true;
  item->SetLabelPreformatted(true);
  item->SetArt("thumb", artworkUrl);

  // Folders carry no columns of their own, a folder not being a track, so the
  // only thing worth showing beyond the name is how much is inside.
  if (node.itemCount > 0)
    item->SetProperty("beefweb.itemcount", node.itemCount);

  items.Add(std::move(item));
}

void CBeefwebDirectory::AddTrack(const BeefwebLibraryNode& node,
                                 const std::string& folderPath,
                                 int64_t startOffset,
                                 const std::string& artworkUrl,
                                 CFileItemList& items)
{
  const BeefwebTrack& track = node.track;

  // The item keeps the file's own path so that Kodi recognises it as audio and
  // so that the remote player is given something it can open. Tracks of a disc
  // image or cue sheet all share that path and are told apart by the subsong
  // number carried alongside it.
  //
  // Deliberately not paired with a dynamic path holding the real name: Kodi
  // judges a disc image by that one too, so keeping it would bring back the
  // very treatment this avoids. The player restores the name instead.
  auto item = std::make_shared<CFileItem>(IdentityPath(track), false);

  // Only a fallback. The label is left for Kodi to write from the tag set
  // below, following whatever the user chose as their track naming template,
  // and marking it preformatted here would have that skipped.
  item->SetLabel(track.title.empty() ? URIUtils::GetFileName(track.path) : track.title);

  item->SetProperty(BEEFWEB_PROPERTY_SUBSONG, track.subsong);

  // The track as the library knows it, which is how it is handed back when
  // queued: the remote player then adds exactly this track, rather than every
  // track of the file it sits in.
  item->SetProperty(BEEFWEB_PROPERTY_PATH, node.path);

  // Naming the art outright also stops Kodi hunting for a picture beside the
  // track, which it cannot reach and which fills the log with complaints
  // about a protocol it has no file handler for.
  item->SetArt("thumb", artworkUrl);

  // Tell tracks of one file apart, which Kodi does in two separate ways.
  //
  // Building a play queue it compares where each track starts, so tracks
  // sharing a file need distinct starts or all but the first are discarded as
  // duplicates. Matching one item against another it compares this property
  // instead, and that comparison has to hold against a track described later
  // from the remote player's own account of what is playing, where the offset
  // is not known. The number of the track within its file is, so the property
  // carries that.
  if (track.subsong > 0)
  {
    item->SetStartOffset(startOffset);
    item->SetProperty("item_start", track.subsong);
  }

  // Play the album through this listing rather than through whatever directory
  // the file happens to sit in. Kodi builds the play queue by listing the
  // item's parent, and the folder on disk holds one file for the whole album,
  // which would collapse it back to a single track.
  item->SetProperty("ParentPath", folderPath);

  MUSIC_INFO::CMusicInfoTag* tag = item->GetMusicInfoTag();
  tag->SetTitle(track.title);
  tag->SetArtist(track.artist);
  tag->SetAlbum(track.album);
  // Falls back to the number within the file, so that an album whose tags are
  // missing still numbers in the order it plays.
  tag->SetTrackNumber(TrackNumberOf(track));
  tag->SetDuration(track.duration);
  tag->SetURL(track.path);
  tag->SetLoaded(true);

  items.Add(std::move(item));
}
