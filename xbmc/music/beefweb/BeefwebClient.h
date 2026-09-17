/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

class CVariant;

namespace KODI::MUSIC::BEEFWEB
{

/*! \brief An entry in the remote player's file browser. */
struct BeefwebEntry
{
  std::string name;
  std::string path; //!< native path on the remote player, passed back verbatim
  bool isFolder{false};
  int64_t size{-1};
  int64_t timestamp{0};
};

/*! \brief A track as resolved by the remote player.
 *
 * Note that \p path is not a unique identifier: container formats such as SACD
 * ISO images and CUE sheets expand into many tracks that all share one path.
 * Tracks are addressed by playlist id plus \p index instead.
 */
struct BeefwebTrack
{
  int index{-1}; //!< index within the playlist it was read from
  std::string artist;
  std::string album;
  std::string title;
  std::string trackNumber;
  std::string codec;
  std::string path; //!< the file on the remote player's disk
  int duration{0}; //!< seconds
  int sampleRate{0};
  int channels{0};
  int bitrate{0}; //!< kbit/s

  /*!
   * \brief Number of the track within its file.
   *
   * Counts from one for disc images and cue sheets, where many tracks share a
   * single file, and is zero for a file holding a single track. Together with
   * the path it identifies a track uniquely.
   */
  int subsong{0};
};

/*! \brief A folder or track in the remote library's folder view. */
struct BeefwebLibraryNode
{
  bool isFolder{false};
  std::string name;

  /*!
   * \brief Where the node sits in the remote library.
   *
   * Relative to the library's own folders rather than to any disk, with a
   * forward slash between folders on every platform. It addresses nothing on
   * disk by itself; the remote player resolves it.
   */
  std::string path;

  int itemCount{0}; //!< recursive track count, folders only
  BeefwebTrack track; //!< tracks only
};

/*! \brief One level of the remote library's folder view. */
struct BeefwebFolderListing
{
  std::vector<BeefwebLibraryNode> nodes;
  std::string path;
  std::string parentPath;
  bool hasParent{false}; //!< false at the root, where there is nowhere to go up to
  int totalCount{0};
};

/*!
 * \brief A track of the remote library, as it is named when handing it back.
 *
 * A file alone would stand for every track in it, which for a disc image is
 * the whole disc, so the number within the file always goes with it.
 */
struct BeefwebLibraryRef
{
  std::string path; //!< library path of the file, as in BeefwebLibraryNode
  int subsong{0};
};

struct BeefwebPlaylist
{
  std::string id;
  std::string title;
  int index{-1};
  int itemCount{0};
  bool isCurrent{false};
};

struct BeefwebPlayerInfo
{
  std::string name;
  std::string title;
  std::string version;
  std::string pluginVersion;
};

enum class PlaybackState
{
  UNKNOWN,
  STOPPED,
  PLAYING,
  PAUSED,
};

struct BeefwebPlayerState
{
  PlaybackState state{PlaybackState::UNKNOWN};
  std::string playlistId;
  int itemIndex{-1};
  double position{0.0}; //!< seconds
  double duration{0.0}; //!< seconds
  double volume{0.0};
  double volumeMin{0.0};
  double volumeMax{0.0};
  bool isMuted{false};
};

/*!
 * \brief Client for the Beefweb remote control API, as provided by the Beefweb
 *        plugin for foobar2000 and DeaDBeeF.
 *
 * The client is stateless apart from the server address and performs one
 * blocking HTTP request per call, so it must not be used from the rendering
 * thread.
 */
class CBeefwebClient
{
public:
  CBeefwebClient() = default;
  CBeefwebClient(std::string host, int port);

  void SetServer(std::string host, int port);

  /*! \brief Configure from the current music player settings. */
  void SetServerFromSettings();

  bool IsConfigured() const { return !m_host.empty() && m_port > 0; }

  /*! \brief Base URL of the API, without a trailing slash. */
  std::string GetBaseUrl() const;

  /*! \brief Query the remote player, doubling as a connection test. */
  bool GetPlayerInfo(BeefwebPlayerInfo& info) const;

  bool GetPlayerState(BeefwebPlayerState& state) const;

  /*!
   * \brief Parse the "player" node of an API response into a player state.
   *
   * Shared with CBeefwebEventListener: the event stream delivers payloads of
   * exactly the same shape as a /player response.
   */
  static bool ParsePlayerState(const CVariant& player, BeefwebPlayerState& state);

  /*! \brief List the roots configured in the remote player's music directories.
   *  \param pathSeparator receives the remote player's path separator.
   */
  bool GetRoots(std::vector<BeefwebEntry>& entries, std::string& pathSeparator) const;

  /*! \brief List the contents of a remote directory.
   *  \param path a native remote path, as returned by GetRoots or GetEntries.
   */
  bool GetEntries(const std::string& path, std::vector<BeefwebEntry>& entries) const;

  bool GetPlaylists(std::vector<BeefwebPlaylist>& playlists) const;

  /*! \brief Look up a playlist by its title. */
  bool FindPlaylist(const std::string& title, std::string& playlistId) const;

  bool CreatePlaylist(const std::string& title, std::string& playlistId) const;

  /*!
   * \brief Find the playlist with the given title, creating it if missing.
   *
   * Used to keep everything Kodi queues in a playlist of its own, so that the
   * playlists the user maintains in the remote player are never touched.
   */
  bool FindOrCreatePlaylist(const std::string& title, std::string& playlistId) const;

  bool ClearPlaylist(const std::string& playlistId) const;

  /*! \brief Read resolved tracks from a playlist.
   *  \param offset first item to read.
   *  \param count number of items to read, or -1 for all remaining.
   *  \param totalCount receives the total number of items in the playlist.
   */
  bool GetPlaylistItems(const std::string& playlistId,
                        int offset,
                        int count,
                        std::vector<BeefwebTrack>& tracks,
                        int& totalCount) const;

  /*!
   * \brief Test whether the remote player has a media library to browse.
   *
   * Not every player offers one, and not every build of the remote control
   * interface exposes it, so callers should fall back to browsing the file
   * system when this is false.
   */
  bool HasLibrary() const;

  /*!
   * \brief Read tracks from the remote player's media library as one list.
   * \param query a query in the remote player's own syntax, empty for all.
   * \param count number of items to read, or -1 for all remaining.
   */
  bool GetLibraryItems(const std::string& query,
                       int offset,
                       int count,
                       std::vector<BeefwebTrack>& tracks,
                       int& totalCount) const;

  /*!
   * \brief List one level of the remote library, arranged as folders.
   * \param path a library path from an earlier listing, empty for the top.
   * \param query filters tracks before the tree is built, empty for all.
   * \param count number of nodes to read, or -1 for all remaining.
   */
  bool GetLibraryFolder(const std::string& path,
                        const std::string& query,
                        int offset,
                        int count,
                        BeefwebFolderListing& listing) const;

  /*! \brief Add remote paths to a playlist, letting the remote player expand
   *         containers such as SACD images and CUE sheets into tracks.
   *  \param replace clear the playlist first.
   *  \param play start playback of the first added item.
   */
  bool AddPaths(const std::string& playlistId,
                const std::vector<std::string>& paths,
                bool replace,
                bool play) const;

  /*!
   * \brief Add tracks of the remote library to a playlist, exactly as named.
   *
   * Unlike AddPaths, a single track of a disc image or cue sheet can be added
   * on its own, and the tracks need not lie in the folders the remote control
   * interface is allowed to open, the remote player resolving them itself.
   *
   * \param tracks the tracks to add. Refused when empty or when any of them
   *        is missing its path, rather than sent: the remote player reads a
   *        request naming nothing as one for everything in the library.
   * \param replace clear the playlist first.
   * \param play start playback of the first added track.
   */
  bool AddFromLibrary(const std::string& playlistId,
                      const std::vector<BeefwebLibraryRef>& tracks,
                      bool replace,
                      bool play) const;

  bool PlayItem(const std::string& playlistId, int index) const;
  bool Play() const;
  bool PlayPause() const;
  bool Pause() const;
  bool Stop() const;
  bool Next() const;
  bool Previous() const;

  bool SetVolume(double volume) const;
  bool SetMuted(bool muted) const;
  bool Seek(double position) const;

private:
  bool Get(const std::string& path, const std::string& query, CVariant& response) const;

  /*! \param response optional, receives the parsed response body if any. */
  bool Post(const std::string& path,
            const std::string& body,
            CVariant* response = nullptr) const;

  std::string m_host;
  int m_port{0};
};

} // namespace KODI::MUSIC::BEEFWEB
