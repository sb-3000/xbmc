/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BeefwebClient.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "filesystem/CurlFile.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"

#include <cstdlib>
#include <limits>
#include <utility>

using namespace KODI::MUSIC::BEEFWEB;

namespace
{
// Connection timeout in seconds. The remote player is normally on the local
// network, so failures should surface quickly rather than block browsing.
constexpr int CONNECT_TIMEOUT = 5;

// Title format columns requested for playlist items, in the order they are
// read back out of the "columns" array of each item.
// Note that the channel count is asked for as a number rather than as
// %channels%, which gives a description such as "stereo" or "5ch".
constexpr auto TRACK_COLUMNS = "%artist%,%album%,%title%,%tracknumber%,%length_seconds%,%codec%,"
                               "%samplerate%,%path%,%subsong%,$info(channels),%bitrate%";

enum TrackColumn
{
  COLUMN_ARTIST = 0,
  COLUMN_ALBUM,
  COLUMN_TITLE,
  COLUMN_TRACKNUMBER,
  COLUMN_LENGTH_SECONDS,
  COLUMN_CODEC,
  COLUMN_SAMPLERATE,
  COLUMN_PATH,
  COLUMN_SUBSONG,
  COLUMN_CHANNELS,
  COLUMN_BITRATE,
  COLUMN_COUNT,
};

/*! \brief Read one item of a playlist or library response into a track. */
bool ParseTrackColumns(const CVariant& item, BeefwebTrack& track)
{
  const CVariant& columns = item["columns"];
  if (!columns.isArray() || columns.size() < static_cast<unsigned int>(COLUMN_COUNT))
    return false;

  track.artist = columns[COLUMN_ARTIST].asString();
  track.album = columns[COLUMN_ALBUM].asString();
  track.title = columns[COLUMN_TITLE].asString();
  track.trackNumber = columns[COLUMN_TRACKNUMBER].asString();
  track.codec = columns[COLUMN_CODEC].asString();
  track.path = columns[COLUMN_PATH].asString();
  track.duration = std::atoi(columns[COLUMN_LENGTH_SECONDS].asString().c_str());
  track.sampleRate = std::atoi(columns[COLUMN_SAMPLERATE].asString().c_str());
  track.subsong = std::atoi(columns[COLUMN_SUBSONG].asString().c_str());

  // Fields the remote player cannot work out come back as "?", which reads as
  // zero and is treated as not known.
  track.channels = std::atoi(columns[COLUMN_CHANNELS].asString().c_str());
  track.bitrate = std::atoi(columns[COLUMN_BITRATE].asString().c_str());

  return true;
}

/*! \brief Format an "offset:count" range, where a negative count means all. */
std::string FormatRange(int offset, int count)
{
  return StringUtils::Format("{}:{}", offset,
                             count < 0 ? std::numeric_limits<int>::max() : count);
}

/*!
 * \brief Range parameter for the library endpoints, which take it in the query
 *        string rather than in the path as playlists do.
 *
 * Left out when everything is wanted, the library endpoints returning every
 * item by default, which reads more plainly than asking for an enormous count.
 * \return the parameter with its leading separator, or an empty string.
 */
std::string RangeParam(int offset, int count)
{
  if (offset <= 0 && count < 0)
    return {};

  return "&range=" + FormatRange(offset, count);
}

PlaybackState ParsePlaybackState(const std::string& state)
{
  if (state == "playing")
    return PlaybackState::PLAYING;
  if (state == "paused")
    return PlaybackState::PAUSED;
  if (state == "stopped")
    return PlaybackState::STOPPED;

  return PlaybackState::UNKNOWN;
}

void ParseEntries(const CVariant& list, std::vector<BeefwebEntry>& entries)
{
  for (auto it = list.begin_array(); it != list.end_array(); ++it)
  {
    BeefwebEntry entry;
    entry.name = (*it)["name"].asString();
    entry.path = (*it)["path"].asString();
    entry.isFolder = (*it)["type"].asString() == "D";
    entry.size = (*it)["size"].asInteger(-1);
    entry.timestamp = (*it)["timestamp"].asInteger(0);

    if (!entry.path.empty())
      entries.emplace_back(std::move(entry));
  }
}
} // unnamed namespace

CBeefwebClient::CBeefwebClient(std::string host, int port)
  : m_host(std::move(host)), m_port(port)
{
}

void CBeefwebClient::SetServer(std::string host, int port)
{
  m_host = std::move(host);
  m_port = port;
}

void CBeefwebClient::SetServerFromSettings()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  m_host = settings->GetString(CSettings::SETTING_MUSICPLAYER_EXTERNALPLAYERHOST);
  m_port = settings->GetInt(CSettings::SETTING_MUSICPLAYER_EXTERNALPLAYERPORT);
}

std::string CBeefwebClient::GetBaseUrl() const
{
  return StringUtils::Format("http://{}:{}/api", m_host, m_port);
}

bool CBeefwebClient::Get(const std::string& path,
                         const std::string& query,
                         CVariant& response) const
{
  if (!IsConfigured())
  {
    CLog::LogF(LOGERROR, "no external player address configured");
    return false;
  }

  std::string url = GetBaseUrl() + path;
  if (!query.empty())
    url += "?" + query;

  XFILE::CCurlFile curl;
  curl.SetTimeout(CONNECT_TIMEOUT);
  curl.SetRequestHeader("Accept", "application/json");

  std::string content;
  if (!curl.Get(url, content))
  {
    CLog::LogF(LOGERROR, "request to {}:{}{} failed", m_host, m_port, path);
    return false;
  }

  if (!CJSONVariantParser::Parse(content, response))
  {
    CLog::LogF(LOGERROR, "invalid JSON in response from {}", path);
    return false;
  }

  return true;
}

bool CBeefwebClient::Post(const std::string& path,
                          const std::string& body,
                          CVariant* response) const
{
  if (!IsConfigured())
  {
    CLog::LogF(LOGERROR, "no external player address configured");
    return false;
  }

  XFILE::CCurlFile curl;
  curl.SetTimeout(CONNECT_TIMEOUT);
  curl.SetMimeType("application/json");

  std::string content;
  if (!curl.Post(GetBaseUrl() + path, body, content))
  {
    CLog::LogF(LOGERROR, "request to {}:{}{} failed", m_host, m_port, path);
    return false;
  }

  // Most commands answer with an empty body, so only parse when the caller
  // wants the response and something came back.
  if (response && !content.empty() && !CJSONVariantParser::Parse(content, *response))
  {
    CLog::LogF(LOGERROR, "invalid JSON in response from {}", path);
    return false;
  }

  return true;
}

bool CBeefwebClient::GetPlayerInfo(BeefwebPlayerInfo& info) const
{
  CVariant response;
  if (!Get("/player", "", response))
    return false;

  const CVariant& player = response["player"]["info"];
  if (!player.isObject())
    return false;

  info.name = player["name"].asString();
  info.title = player["title"].asString();
  info.version = player["version"].asString();
  info.pluginVersion = player["pluginVersion"].asString();

  return true;
}

bool CBeefwebClient::GetPlayerState(BeefwebPlayerState& state) const
{
  CVariant response;
  if (!Get("/player", "", response))
    return false;

  return ParsePlayerState(response["player"], state);
}

bool CBeefwebClient::ParsePlayerState(const CVariant& player, BeefwebPlayerState& state)
{
  if (!player.isObject())
    return false;

  state.state = ParsePlaybackState(player["playbackState"].asString());

  const CVariant& item = player["activeItem"];
  state.playlistId = item["playlistId"].asString();
  state.itemIndex = item["index"].asInteger32(-1);
  state.position = item["position"].asDouble(0.0);
  state.duration = item["duration"].asDouble(0.0);

  const CVariant& volume = player["volume"];
  state.volume = volume["value"].asDouble(0.0);
  state.volumeMin = volume["min"].asDouble(0.0);
  state.volumeMax = volume["max"].asDouble(0.0);
  state.isMuted = volume["isMuted"].asBoolean(false);

  return true;
}

bool CBeefwebClient::GetRoots(std::vector<BeefwebEntry>& entries,
                              std::string& pathSeparator) const
{
  CVariant response;
  if (!Get("/browser/roots", "", response))
    return false;

  pathSeparator = response["pathSeparator"].asString();
  ParseEntries(response["roots"], entries);

  return true;
}

bool CBeefwebClient::GetEntries(const std::string& path,
                                std::vector<BeefwebEntry>& entries) const
{
  CVariant response;
  if (!Get("/browser/entries", "path=" + CURL::Encode(path), response))
    return false;

  ParseEntries(response["entries"], entries);

  return true;
}

bool CBeefwebClient::GetPlaylists(std::vector<BeefwebPlaylist>& playlists) const
{
  CVariant response;
  if (!Get("/playlists", "", response))
    return false;

  const CVariant& list = response["playlists"];
  for (auto it = list.begin_array(); it != list.end_array(); ++it)
  {
    BeefwebPlaylist playlist;
    playlist.id = (*it)["id"].asString();
    playlist.title = (*it)["title"].asString();
    playlist.index = (*it)["index"].asInteger32(-1);
    playlist.itemCount = (*it)["itemCount"].asInteger32(0);
    playlist.isCurrent = (*it)["isCurrent"].asBoolean(false);

    if (!playlist.id.empty())
      playlists.emplace_back(std::move(playlist));
  }

  return true;
}

bool CBeefwebClient::FindPlaylist(const std::string& title, std::string& playlistId) const
{
  std::vector<BeefwebPlaylist> playlists;
  if (!GetPlaylists(playlists))
    return false;

  for (const auto& playlist : playlists)
  {
    if (playlist.title == title)
    {
      playlistId = playlist.id;
      return true;
    }
  }

  return false;
}

bool CBeefwebClient::CreatePlaylist(const std::string& title, std::string& playlistId) const
{
  CVariant request(CVariant::VariantTypeObject);
  request["title"] = title;
  // Leave the remote player's current playlist alone; playback is started by
  // explicit item index rather than by relying on which playlist is current.
  request["setCurrent"] = false;

  std::string body;
  if (!CJSONVariantWriter::Write(request, body, true))
    return false;

  CVariant response;
  if (!Post("/playlists/add", body, &response))
    return false;

  playlistId = response["id"].asString();
  if (playlistId.empty())
  {
    CLog::LogF(LOGERROR, "created playlist '{}' but got no id back", title);
    return false;
  }

  return true;
}

bool CBeefwebClient::FindOrCreatePlaylist(const std::string& title, std::string& playlistId) const
{
  if (FindPlaylist(title, playlistId))
    return true;

  return CreatePlaylist(title, playlistId);
}

bool CBeefwebClient::ClearPlaylist(const std::string& playlistId) const
{
  return Post(StringUtils::Format("/playlists/{}/clear", CURL::Encode(playlistId)), "");
}

bool CBeefwebClient::GetPlaylistItems(const std::string& playlistId,
                                      int offset,
                                      int count,
                                      std::vector<BeefwebTrack>& tracks,
                                      int& totalCount) const
{
  // The API addresses items by a "offset:count" range. Beefweb clamps a range
  // that runs past the end of the playlist, so asking for everything is safe.
  CVariant response;
  if (!Get(StringUtils::Format("/playlists/{}/items/{}", CURL::Encode(playlistId),
                               FormatRange(offset, count)),
           std::string("columns=") + CURL::Encode(TRACK_COLUMNS), response))
  {
    return false;
  }

  const CVariant& items = response["playlistItems"]["items"];
  if (!items.isArray())
    return false;

  totalCount = response["playlistItems"]["totalCount"].asInteger32(0);

  int index = response["playlistItems"]["offset"].asInteger32(offset);
  for (auto it = items.begin_array(); it != items.end_array(); ++it)
  {
    BeefwebTrack track;
    if (ParseTrackColumns(*it, track))
    {
      track.index = index;
      tracks.emplace_back(std::move(track));
    }

    ++index;
  }

  return true;
}

bool CBeefwebClient::HasLibrary() const
{
  // Players without a media library answer with 501, and builds predating the
  // library interface with 404, either of which surfaces as a failed request.
  CVariant response;
  if (!Get("/library/info", "", response))
    return false;

  const CVariant& library = response["library"];

  return library["supported"].asBoolean(false) && library["enabled"].asBoolean(false);
}

bool CBeefwebClient::GetLibraryItems(const std::string& query,
                                     int offset,
                                     int count,
                                     std::vector<BeefwebTrack>& tracks,
                                     int& totalCount) const
{
  std::string params = std::string("columns=") + CURL::Encode(TRACK_COLUMNS);
  if (!query.empty())
    params += "&query=" + CURL::Encode(query);
  params += RangeParam(offset, count);

  CVariant response;
  if (!Get("/library/items", params, response))
    return false;

  const CVariant& items = response["libraryItems"]["items"];
  if (!items.isArray())
    return false;

  totalCount = response["libraryItems"]["totalCount"].asInteger32(0);

  for (auto it = items.begin_array(); it != items.end_array(); ++it)
  {
    BeefwebTrack track;
    if (!ParseTrackColumns(*it, track))
      continue;

    // The number within the file comes as a field of its own, which is the
    // one to trust over the column asking for the same thing.
    if (it->isMember("subsong"))
      track.subsong = (*it)["subsong"].asInteger32(track.subsong);

    tracks.emplace_back(std::move(track));
  }

  return true;
}

bool CBeefwebClient::GetLibraryFolder(const std::string& path,
                                      const std::string& query,
                                      int offset,
                                      int count,
                                      BeefwebFolderListing& listing) const
{
  std::string params = std::string("columns=") + CURL::Encode(TRACK_COLUMNS);
  if (!path.empty())
    params += "&path=" + CURL::Encode(path);
  if (!query.empty())
    params += "&query=" + CURL::Encode(query);
  params += RangeParam(offset, count);

  CVariant response;
  if (!Get("/library/items/by-path", params, response))
    return false;

  const CVariant& level = response["libraryNodes"];
  const CVariant& items = level["items"];
  if (!items.isArray())
    return false;

  listing.totalCount = level["totalCount"].asInteger32(0);
  listing.path = level["path"].asString();

  // The key is absent at the root. An empty value is a real path, being the
  // root itself, so the two cases have to be told apart by presence.
  listing.hasParent = level.isMember("parentPath");
  if (listing.hasParent)
    listing.parentPath = level["parentPath"].asString();

  for (auto it = items.begin_array(); it != items.end_array(); ++it)
  {
    BeefwebLibraryNode node;
    node.isFolder = (*it)["type"].asString() == "D";
    node.name = (*it)["name"].asString();
    node.path = (*it)["path"].asString();

    if (node.isFolder)
    {
      node.itemCount = (*it)["itemCount"].asInteger32(0);
    }
    else
    {
      if (!ParseTrackColumns(*it, node.track))
        continue; // a track whose columns did not come back in the expected shape

      // As for the flat list, the field is the one to trust.
      if (it->isMember("subsong"))
        node.track.subsong = (*it)["subsong"].asInteger32(node.track.subsong);
    }

    listing.nodes.emplace_back(std::move(node));
  }

  return true;
}

bool CBeefwebClient::AddPaths(const std::string& playlistId,
                              const std::vector<std::string>& paths,
                              bool replace,
                              bool play) const
{
  if (paths.empty())
    return false;

  CVariant request(CVariant::VariantTypeObject);
  request["items"] = CVariant(CVariant::VariantTypeArray);
  for (const auto& path : paths)
    request["items"].push_back(path);

  request["replace"] = replace;
  request["play"] = play;
  // Resolving a folder or a container such as an SACD image can take a moment;
  // wait for it so that the playlist is populated when this call returns.
  request["async"] = false;

  std::string body;
  if (!CJSONVariantWriter::Write(request, body, true))
    return false;

  return Post(StringUtils::Format("/playlists/{}/items/add", CURL::Encode(playlistId)), body);
}

bool CBeefwebClient::AddFromLibrary(const std::string& playlistId,
                                    const std::vector<BeefwebLibraryRef>& tracks,
                                    bool replace,
                                    bool play) const
{
  // Refuse outright rather than send any of this. The remote player reads a
  // request that names no items as one for everything its query matches, and
  // with no query that is the entire library. An entry without a path would
  // name the top of the library, which comes to the same thing.
  if (tracks.empty())
    return false;

  CVariant request(CVariant::VariantTypeObject);
  request["items"] = CVariant(CVariant::VariantTypeArray);

  for (const auto& track : tracks)
  {
    if (track.path.empty())
    {
      CLog::LogF(LOGERROR, "refusing to add a library track that has no path");
      return false;
    }

    CVariant entry(CVariant::VariantTypeObject);
    entry["path"] = track.path;
    // Always given, zero included: a file named without it stands for every
    // track in it, which for a disc image is the whole disc.
    entry["subsong"] = track.subsong;
    request["items"].push_back(std::move(entry));
  }

  request["replace"] = replace;
  request["play"] = play;

  std::string body;
  if (!CJSONVariantWriter::Write(request, body, true))
    return false;

  return Post(
      StringUtils::Format("/playlists/{}/items/add-from-library", CURL::Encode(playlistId)),
      body);
}

bool CBeefwebClient::PlayItem(const std::string& playlistId, int index) const
{
  return Post(StringUtils::Format("/player/play/{}/{}", CURL::Encode(playlistId), index), "");
}

bool CBeefwebClient::Play() const
{
  return Post("/player/play", "");
}

bool CBeefwebClient::PlayPause() const
{
  return Post("/player/play-pause", "");
}

bool CBeefwebClient::Pause() const
{
  return Post("/player/pause", "");
}

bool CBeefwebClient::Stop() const
{
  return Post("/player/stop", "");
}

bool CBeefwebClient::Next() const
{
  return Post("/player/next", "");
}

bool CBeefwebClient::Previous() const
{
  return Post("/player/previous", "");
}

bool CBeefwebClient::SetVolume(double volume) const
{
  CVariant request(CVariant::VariantTypeObject);
  request["volume"] = volume;

  std::string body;
  if (!CJSONVariantWriter::Write(request, body, true))
    return false;

  return Post("/player", body);
}

bool CBeefwebClient::SetMuted(bool muted) const
{
  CVariant request(CVariant::VariantTypeObject);
  request["isMuted"] = muted;

  std::string body;
  if (!CJSONVariantWriter::Write(request, body, true))
    return false;

  return Post("/player", body);
}

bool CBeefwebClient::Seek(double position) const
{
  CVariant request(CVariant::VariantTypeObject);
  request["position"] = position;

  std::string body;
  if (!CJSONVariantWriter::Write(request, body, true))
    return false;

  return Post("/player", body);
}
