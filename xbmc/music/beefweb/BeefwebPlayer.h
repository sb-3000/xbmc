/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BeefwebClient.h"
#include "BeefwebEventListener.h"
#include "cores/IPlayer.h"
#include "threads/CriticalSection.h"
#include "threads/Thread.h"

#include <chrono>
#include <optional>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace KODI::MUSIC::BEEFWEB
{

/*!
 * \brief Item property holding the subsong number of a library track.
 *
 * Set when browsing the remote library, where tracks of a disc image or cue
 * sheet share one path and are told apart by this number.
 */
constexpr auto BEEFWEB_PROPERTY_SUBSONG = "beefweb.subsong";

/*!
 * \brief Item property holding the path of a track in the remote library.
 *
 * Set alongside the subsong number when browsing the remote library, so that
 * a queued track can be handed back as the library entry it came from.
 */
constexpr auto BEEFWEB_PROPERTY_PATH = "beefweb.path";

/*!
 * \brief Extensions added to tracks Kodi would otherwise not take for audio.
 *
 * Listings are filtered against the extensions Kodi can play itself, and disc
 * images are treated as something to browse into rather than something to
 * play. Neither is the right question when the playing is done elsewhere, so
 * such a track is known within Kodi by a name ending in one of these instead,
 * and the real name is restored before the remote player is given it.
 *
 * Nothing on disk carries either. Both are added to the list of music
 * extensions while an external player is in use. Which one is used names the
 * medium where that can be established, because wherever Kodi shows a file's
 * type it shows the extension.
 *
 * @{
 */
constexpr auto BEEFWEB_SUFFIX_SACD = ".sacd";
constexpr auto BEEFWEB_SUFFIX_AUDIO = ".audio";
/*! @} */

/*!
 * \brief Whether a codec name describes direct stream digital audio.
 *
 * Reported as DSD followed by a rate, or DST where the stream is losslessly
 * packed. Held in a disc image, that is what distinguishes a super audio CD
 * from any other kind.
 */
bool IsDirectStreamDigital(const std::string& codec);

/*! \brief The medium a track came from, as far as it can be established. */
std::string MediumSuffix(const std::string& remotePath, const std::string& codec);

/*! \brief Values of the "musicplayer.externalplayer" setting. */
enum ExternalMusicPlayer
{
  EXTERNAL_MUSIC_PLAYER_NONE = 0, //!< Kodi's built-in player
  EXTERNAL_MUSIC_PLAYER_BEEFWEB = 1,
};

/*!
 * \brief Values of the "musicplayer.externalplayerarea" setting.
 *
 * A disc may hold the same music mixed more than once, which the remote
 * library reports as separate tracks. Choosing an area keeps one of them.
 */
enum ExternalPlayerArea
{
  EXTERNAL_PLAYER_AREA_NONE = 0, //!< show whatever the library holds
  EXTERNAL_PLAYER_AREA_STEREO = 1,
  EXTERNAL_PLAYER_AREA_MULTICHANNEL = 2,
};

/*!
 * \brief Plays music through a remote Beefweb-controlled player.
 *
 * No audio passes through Kodi: the remote player opens the file itself and
 * drives its own output device, which is the point of the exercise on setups
 * where the remote player reaches the hardware in a way Kodi cannot.
 *
 * The remote player owns playback. Kodi's queue is handed over whole when
 * playback starts, and from then on the remote player walks through it at its
 * own pace, which is what allows an album held in a single file to play
 * without gaps. Kodi follows: as tracks change it is told what is playing so
 * that it can show it, but its own queue does not advance, there being only
 * one thing deciding what comes next.
 */
class CBeefwebPlayer : public IPlayer, public IBeefwebEventHandler, private CThread
{
public:
  explicit CBeefwebPlayer(IPlayerCallback& callback);
  ~CBeefwebPlayer() override;

  // IPlayer
  bool OpenFile(const CFileItem& file, const CPlayerOptions& options) override;
  bool CloseFile(bool reopen = false) override;
  bool IsPlaying() const override;
  bool CanPause() const override { return true; }
  void Pause() override;
  bool HasVideo() const override { return false; }
  bool HasAudio() const override { return true; }
  bool CanSeek() const override { return true; }
  void Seek(bool bPlus, bool bLargeStep, bool bChapterOverride) override;
  void SeekTime(int64_t iTime) override;
  bool SeekTimeRelative(int64_t iTime) override;
  void SeekPercentage(float fPercent) override;
  void SetSpeed(float speed) override;
  void SetVolume(float volume) override;
  void SetMute(bool mute) override;

  int GetAudioStreamCount() const override { return m_playing ? 1 : 0; }
  int GetAudioStream() override { return m_playing ? 0 : -1; }

  /*!
   * \brief Describe what the remote player is playing.
   *
   * Without this Kodi falls back on guessing from the file name, which for a
   * track renamed to get past its own ideas about extensions means it reports
   * the invented one.
   */
  void GetAudioStreamInfo(int index, AudioStreamInfo& info) const override;

  // IBeefwebEventHandler
  void OnBeefwebStateChanged(const BeefwebPlayerState& state) override;
  void OnBeefwebConnectionChanged(bool connected) override;

  /*!
   * \brief Convert a Kodi path into one the remote player can open.
   *
   * The remote player runs as a separate application with no knowledge of
   * Kodi's virtual file system, so only paths that exist for both are usable.
   * \return false if the path cannot be handed over.
   */
  static bool TranslatePath(const std::string& kodiPath, std::string& remotePath);

private:
  /*!
   * \brief Keeps the elapsed time moving between events.
   *
   * The remote player reports a position only when something happens to it,
   * never as a steady tick, and Kodi shows whatever time it was last given.
   * Without this the elapsed time would sit still for the length of a track.
   */
  void Process() override;

  /*! \brief Note the position the remote player has just reported. */
  void AnchorPlayTimes(const BeefwebPlayerState& state);

  /*! \brief Publish the elapsed time, carried forward to now, to the GUI. */
  void PublishPlayTimes();

  /*! \brief Whether contact has been lost for too long to be a momentary drop. */
  bool HasLostPlayer() const;

  /*! \brief Note that the remote player is stopped, or that it is not. */
  void NoteStopped(bool stopped);

  /*! \brief Whether it has stayed stopped for longer than a gap between tracks. */
  bool HasStoppedForGood() const;

  /*!
   * \brief What Kodi has queued, in the form the remote player is given it.
   *
   * Tracks browsed from the remote library are named one by one, as the
   * library entries they came from. Whole files are collected as well, being
   * the only form anything else Kodi can queue takes, and a way back should
   * the remote player refuse the tracks.
   */
  struct Queue
  {
    std::vector<BeefwebLibraryRef> tracks; //!< empty unless every item came from the library
    std::vector<std::string> files; //!< remote paths, each file once

    /*! \brief Identifies the queue, to tell whether it is loaded already. */
    std::vector<std::string> Key() const;
  };

  /*! \brief Collect what Kodi has queued, in order. */
  Queue BuildQueue(const CFileItem& file) const;

  /*!
   * \brief Hand a queue to the remote player, unless it already has it.
   *
   * Reloading would interrupt playback, so an unchanged queue is left alone.
   * This is what lets a track be chosen from an album that is already loaded
   * without the album starting over.
   */
  bool LoadQueue(const Queue& queue);

  /*! \brief Tell Kodi which track is playing, without moving its queue. */
  void ReportTrack(const BeefwebTrack& track);

  /*! \brief Note the track now playing and let Kodi know its details changed. */
  void SetCurrentTrack(int index);

  /*!
   * \brief Position of a track within the loaded playlist.
   * \param subsong number of the track within its file, zero if it is a file
   *        of its own.
   * \return the position, or -1 if the track is not in the playlist.
   */
  int FindTrackIndex(const std::string& remotePath, int subsong) const;

  /*! \brief Forget the loaded playlist, so that the next file reloads it. */
  void InvalidateLoaded();

  CBeefwebClient m_client;
  std::unique_ptr<CBeefwebEventListener> m_listener;

  /*! \brief Id of the playlist Kodi owns on the remote player. */
  std::string m_playlistId;

  /*! \brief Identifies the queue currently handed over, empty if none. */
  std::vector<std::string> m_loadedQueue;

  /*! \brief Tracks those files resolved into, in playlist order. */
  std::vector<BeefwebTrack> m_loadedTracks;

  mutable CCriticalSection m_stateSection;
  PlaybackState m_lastState{PlaybackState::UNKNOWN};

  /*! \brief Position of the track the remote player last reported on. */
  int m_lastItemIndex{-1};

  /*! \brief What is playing, as the remote player described it. */
  BeefwebTrack m_currentTrack;

  /*! \brief Last reported position and the moment it was reported. */
  mutable CCriticalSection m_timeSection;
  double m_anchorPosition{0.0};
  double m_anchorDuration{0.0};
  std::chrono::steady_clock::time_point m_anchorAt;
  bool m_anchorAdvancing{false};

  /*! \brief When contact was lost, unset while the player is reachable. */
  std::optional<std::chrono::steady_clock::time_point> m_disconnectedAt;

  /*! \brief When the remote player last reported stopping, unset if playing. */
  std::optional<std::chrono::steady_clock::time_point> m_stoppedAt;

  /*! \brief Set between a successful OpenFile and the end of that track. */
  std::atomic<bool> m_playing{false};

  /*! \brief Speed reported to Kodi. The remote player only plays or pauses. */
  std::atomic<float> m_speed{0.0f};
};

} // namespace KODI::MUSIC::BEEFWEB
