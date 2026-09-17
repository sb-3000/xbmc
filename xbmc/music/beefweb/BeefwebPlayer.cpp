/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BeefwebPlayer.h"

#include "FileItem.h"
#include "PlayListPlayer.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "cores/DataCacheCore.h"
#include "cores/IPlayerCallback.h"
#include "music/tags/MusicInfoTag.h"
#include "playlists/PlayList.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <mutex>

using namespace KODI::MUSIC::BEEFWEB;

namespace
{
// Title of the playlist Kodi creates on the remote player. Keeping everything
// Kodi queues in a playlist of its own means the playlists the user maintains
// there are never cleared or reordered behind their back.
constexpr auto KODI_PLAYLIST_TITLE = "Kodi";

constexpr int64_t SECONDS_TO_MS = 1000;

// How often the elapsed time is refreshed between events from the remote
// player. Frequent enough that the progress display moves smoothly, and far
// cheaper than it sounds since it touches nothing but a cached value.
constexpr auto PROGRESS_INTERVAL = std::chrono::milliseconds(250);

// How long the remote player may stay out of contact before playback is given
// up on. Long enough to ride out a restart of its remote control interface,
// short enough that a player which has really gone does not leave Kodi showing
// a track for ever.
constexpr auto RECONNECT_GRACE = std::chrono::seconds(30);

// How long the remote player may read as stopped before that is taken to mean
// playback is over. Moving from one track to the next can show up as a moment
// of it, and reporting that would leave Kodi believing playback had finished
// while the remote player carried on into the next track.
constexpr auto STOP_GRACE = std::chrono::milliseconds(1500);

/*! \brief Step used when seeking without an explicit target. */
constexpr int64_t SEEK_STEP_SMALL = 30 * SECONDS_TO_MS;
constexpr int64_t SEEK_STEP_LARGE = 600 * SECONDS_TO_MS;
} // unnamed namespace

bool KODI::MUSIC::BEEFWEB::IsDirectStreamDigital(const std::string& codec)
{
  return StringUtils::StartsWithNoCase(codec, "DSD") ||
         StringUtils::StartsWithNoCase(codec, "DST");
}

std::string KODI::MUSIC::BEEFWEB::MediumSuffix(const std::string& remotePath,
                                               const std::string& codec)
{
  // A super audio CD is direct stream digital held in a disc image. The
  // extension by itself proves nothing, a video or data image carrying the
  // same one, so the encoding has to agree.
  if (URIUtils::IsDiscImage(remotePath) && IsDirectStreamDigital(codec))
    return BEEFWEB_SUFFIX_SACD;

  return BEEFWEB_SUFFIX_AUDIO;
}

CBeefwebPlayer::CBeefwebPlayer(IPlayerCallback& callback)
  : IPlayer(callback), CThread("BeefwebProgress")
{
  m_client.SetServerFromSettings();
  m_listener = std::make_unique<CBeefwebEventListener>(this);
}

CBeefwebPlayer::~CBeefwebPlayer()
{
  // Tear these down before anything else: both run on their own threads and
  // reach back into this object.
  StopThread(true);
  m_listener.reset();
}

bool CBeefwebPlayer::TranslatePath(const std::string& kodiPath, std::string& remotePath)
{
  if (kodiPath.empty())
    return false;

  // Undo the renaming that got the track past Kodi's ideas about what its
  // name means. The remote player knows it by its real one.
  std::string path(kodiPath);
  for (const auto* suffix : {BEEFWEB_SUFFIX_SACD, BEEFWEB_SUFFIX_AUDIO})
  {
    if (StringUtils::EndsWith(path, suffix))
    {
      path.erase(path.size() - std::char_traits<char>::length(suffix));
      break;
    }
  }

  const CURL url(path);
  const std::string protocol = url.GetProtocol();

  // A plain local path is already something the remote player can open, as
  // long as it really is running on the machine it names.
  if (protocol.empty())
  {
    remotePath = path;
    return true;
  }

  // Kodi addresses network shares through its own virtual file system. The
  // UNC form of the same share is what an ordinary Windows application opens.
  if (protocol == "smb")
  {
    const std::string host = url.GetHostName();
    std::string share = url.GetFileName();
    if (host.empty() || share.empty())
      return false;

    StringUtils::Replace(share, '/', '\\');
    remotePath = "\\\\" + host + "\\" + share;
    return true;
  }

  CLog::LogF(LOGERROR, "cannot hand a '{}' path to the external player", protocol);
  return false;
}

bool CBeefwebPlayer::OpenFile(const CFileItem& file, const CPlayerOptions& options)
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const std::string host =
      settings->GetString(CSettings::SETTING_MUSICPLAYER_EXTERNALPLAYERHOST);
  const int port = settings->GetInt(CSettings::SETTING_MUSICPLAYER_EXTERNALPLAYERPORT);

  m_client.SetServer(host, port);

  if (!m_client.IsConfigured())
  {
    CLog::LogF(LOGERROR, "no external player configured");
    return false;
  }

  std::string remotePath;
  if (!TranslatePath(file.GetDynPath(), remotePath))
    return false;

  // Confirm the remote player is actually there before claiming the file, so
  // that a stopped player surfaces as a failed playback rather than silence.
  BeefwebPlayerInfo info;
  if (!m_client.GetPlayerInfo(info))
  {
    CLog::LogF(LOGERROR, "external player is not reachable");
    return false;
  }

  if (m_playlistId.empty() &&
      !m_client.FindOrCreatePlaylist(KODI_PLAYLIST_TITLE, m_playlistId))
  {
    CLog::LogF(LOGERROR, "could not prepare a playlist on the external player");
    return false;
  }

  // Tracks browsed from the remote library carry the number of the track
  // within their file. Anything else is a whole file of its own.
  const int subsong = static_cast<int>(file.GetProperty(BEEFWEB_PROPERTY_SUBSONG).asInteger(0));

  CLog::LogF(LOGDEBUG, "opening subsong {} of '{}'", subsong, CURL::GetRedacted(remotePath));

  // Hand over everything Kodi has queued, not just this track, so that the
  // remote player can carry on by itself once this one finishes.
  if (!LoadQueue(BuildQueue(file)))
    return false;

  const int index = FindTrackIndex(remotePath, subsong);
  if (index < 0)
  {
    CLog::LogF(LOGERROR, "track {} of '{}' is not in the playlist", subsong,
               CURL::GetRedacted(remotePath));
    InvalidateLoaded();
    return false;
  }

  // Opening a file is now only ever the user choosing something, the remote
  // player moving on by itself being reported to Kodi rather than handed back
  // as a fresh request. So always ask for the track: the close that came just
  // before will have stopped playback, and anything that skipped this would
  // leave the remote player sitting silent.
  if (!m_client.PlayItem(m_playlistId, index))
  {
    CLog::LogF(LOGERROR, "external player could not start '{}'", CURL::GetRedacted(remotePath));
    InvalidateLoaded();
    return false;
  }

  {
    std::unique_lock<CCriticalSection> lock(m_stateSection);
    m_lastState = PlaybackState::UNKNOWN;

    // Note where playback now is, so that the event confirming it is not taken
    // for the remote player having moved on again.
    m_lastItemIndex = index;
  }

  // Forget any earlier stop, or the one that ended the last queue would be
  // taken for this one having ended before it began.
  NoteStopped(false);

  m_playing = true;
  m_speed = 1.0f;

  SetCurrentTrack(index);

  CDataCacheCore::GetInstance().SetSpeed(1.0f, 1.0f);

  m_listener->Start(host, port);

  if (!IsRunning())
    Create();

  m_callback.OnPlayBackStarted(file);
  m_callback.OnAVStarted(file);

  return true;
}

CBeefwebPlayer::Queue CBeefwebPlayer::BuildQueue(const CFileItem& file) const
{
  std::vector<std::shared_ptr<CFileItem>> items;

  const PLAYLIST::CPlayList& playlist =
      CServiceBroker::GetPlaylistPlayer().GetPlaylist(PLAYLIST::TYPE_MUSIC);

  for (int i = 0; i < playlist.size(); ++i)
    items.emplace_back(playlist[i]);

  // Playing a single item on its own leaves Kodi's queue empty.
  if (items.empty())
    items.emplace_back(std::make_shared<CFileItem>(file));

  // Hand library tracks over as themselves when that is all there is. The
  // remote player then adds exactly those, rather than every track of each
  // file they come from, which for a disc holding both a stereo and a
  // multichannel mix would bring back the one filtered out of the listing.
  const bool fromLibrary =
      std::all_of(items.begin(), items.end(), [](const std::shared_ptr<CFileItem>& item)
                  { return item->HasProperty(BEEFWEB_PROPERTY_PATH); });

  Queue queue;

  for (const auto& item : items)
  {
    if (fromLibrary)
    {
      queue.tracks.push_back(
          {item->GetProperty(BEEFWEB_PROPERTY_PATH).asString(),
           static_cast<int>(item->GetProperty(BEEFWEB_PROPERTY_SUBSONG).asInteger(0))});
    }

    // Whole files too, each once: the only form for anything that did not
    // come from the library, and the way back if library tracks are refused.
    std::string remotePath;
    if (TranslatePath(item->GetDynPath(), remotePath) &&
        std::find(queue.files.begin(), queue.files.end(), remotePath) == queue.files.end())
    {
      queue.files.emplace_back(std::move(remotePath));
    }
  }

  return queue;
}

std::vector<std::string> CBeefwebPlayer::Queue::Key() const
{
  std::vector<std::string> key;

  if (!tracks.empty())
  {
    for (const auto& track : tracks)
      key.emplace_back(StringUtils::Format("track:{}#{}", track.path, track.subsong));
  }
  else
  {
    for (const auto& file : files)
      key.emplace_back("file:" + file);
  }

  return key;
}

bool CBeefwebPlayer::LoadQueue(const Queue& queue)
{
  const std::vector<std::string> key = queue.Key();

  // Nothing to hand over. Settled before anything is sent, since a request to
  // add from the library that names nothing is taken to mean all of it.
  if (key.empty())
    return false;

  // The remote player already has exactly this queue, so leave it playing.
  if (m_loadedQueue == key && !m_loadedTracks.empty())
    return true;

  InvalidateLoaded();

  // Replace rather than append: this playlist mirrors what Kodi has queued.
  // Playback is started afterwards by position, so nothing is asked to start
  // here.
  bool added = false;
  if (!queue.tracks.empty())
  {
    added = m_client.AddFromLibrary(m_playlistId, queue.tracks, true, false);

    // A remote player that cannot add from its library, or refuses to, is
    // handed whole files instead. Playback still works, but a disc holding two
    // mixes then plays through both of them.
    if (!added)
      CLog::LogF(LOGWARNING, "external player refused library tracks, handing over files");
  }

  if (!added && !queue.files.empty())
    added = m_client.AddPaths(m_playlistId, queue.files, true, false);

  if (!added)
  {
    CLog::LogF(LOGERROR, "external player refused a queue of {} item(s)", key.size());
    return false;
  }

  // Read back what was added rather than assuming. Only the remote player
  // knows what the files resolved into and in what order.
  int totalCount = 0;
  if (!m_client.GetPlaylistItems(m_playlistId, 0, -1, m_loadedTracks, totalCount))
  {
    CLog::LogF(LOGERROR, "could not read back the playlist after handing over the queue");
    InvalidateLoaded();
    return false;
  }

  m_loadedQueue = key;

  CLog::LogF(LOGDEBUG, "handed over {} item(s) resolving to {} track(s)", key.size(),
             m_loadedTracks.size());

  return true;
}

void CBeefwebPlayer::ReportTrack(const BeefwebTrack& track)
{
  CLog::LogF(LOGDEBUG, "reporting position {} as '{}' by '{}'", track.index, track.title,
             track.artist);

  CFileItem item(track.path, false);
  item.SetLabel(track.title.empty() ? track.path : track.title);

  MUSIC_INFO::CMusicInfoTag* tag = item.GetMusicInfoTag();
  tag->SetTitle(track.title);
  tag->SetArtist(track.artist);
  tag->SetAlbum(track.album);
  tag->SetDuration(track.duration);
  tag->SetURL(track.path);
  tag->SetLoaded(true);

  if (track.subsong > 0)
    item.SetProperty("item_start", track.subsong);

  // Reporting a start is what puts an item in front of the user. It does not
  // move Kodi's queue, which is deliberate: the remote player decides what
  // comes next and Kodi is only being kept informed.
  m_callback.OnPlayBackStarted(item);
  m_callback.OnAVStarted(item);
}

int CBeefwebPlayer::FindTrackIndex(const std::string& remotePath, int subsong) const
{
  // Both this number and the one the track was browsed with come from the same
  // field of the remote player, so they can be compared directly instead of
  // guessing how subsongs map onto playlist positions.
  for (const auto& track : m_loadedTracks)
  {
    if (track.path == remotePath && track.subsong == subsong)
      return track.index;
  }

  // A file holding a single track may report no subsong at all, in which case
  // there is only one candidate anyway.
  if (m_loadedTracks.size() == 1 && m_loadedTracks.front().path == remotePath)
    return m_loadedTracks.front().index;

  return -1;
}

void CBeefwebPlayer::InvalidateLoaded()
{
  m_loadedQueue.clear();
  m_loadedTracks.clear();
}

bool CBeefwebPlayer::CloseFile(bool reopen)
{
  // Clear this before stopping the listener. Stopping it reports the
  // connection as lost, and raising a playback error from inside Kodi's own
  // teardown re-enters the player and deadlocks. With the flag already down
  // both event handlers return without calling back.
  const bool wasPlaying = m_playing.exchange(false);

  CLog::LogF(LOGDEBUG, "closing, was playing: {}", wasPlaying);

  StopThread(true);

  // The listener is deliberately left running. Kodi closes the current file
  // between tracks, and dropping the stream each time would lose sight of the
  // remote player over exactly the moment worth watching. It is shut down when
  // this player is destroyed.

  if (wasPlaying)
  {
    m_client.Stop();

    // Report the stop even though Kodi asked for it. Choosing something new
    // while a track is playing does not open it straight away: Kodi puts the
    // request aside, closes the player, and only opens it once that player
    // says it has finished. Staying silent here leaves the request put aside
    // for ever, which looks exactly like the click having merely stopped
    // playback.
    //
    // Safe to raise from inside the close: the callback only posts a message
    // for the main loop to pick up, so nothing re-enters the player here.
    if (!reopen)
      m_callback.OnPlayBackStopped();
  }

  m_speed = 0.0f;

  return true;
}

bool CBeefwebPlayer::IsPlaying() const
{
  return m_playing;
}

void CBeefwebPlayer::Pause()
{
  if (!m_playing)
    return;

  // The remote player owns the play/pause state, so toggle there and let the
  // resulting event decide what Kodi is told.
  m_client.PlayPause();
}

void CBeefwebPlayer::Seek(bool bPlus, bool bLargeStep, bool bChapterOverride)
{
  const int64_t step = bLargeStep ? SEEK_STEP_LARGE : SEEK_STEP_SMALL;
  SeekTimeRelative(bPlus ? step : -step);
}

void CBeefwebPlayer::SeekTime(int64_t iTime)
{
  if (m_playing)
    m_client.Seek(static_cast<double>(iTime) / SECONDS_TO_MS);
}

bool CBeefwebPlayer::SeekTimeRelative(int64_t iTime)
{
  if (!m_playing)
    return false;

  const BeefwebPlayerState state = m_listener->GetLastState();
  const double target = state.position + static_cast<double>(iTime) / SECONDS_TO_MS;

  m_client.Seek(std::max(0.0, target));

  return true;
}

void CBeefwebPlayer::SeekPercentage(float fPercent)
{
  const BeefwebPlayerState state = m_listener->GetLastState();
  if (state.duration <= 0.0)
    return;

  m_client.Seek(state.duration * static_cast<double>(fPercent) / 100.0);
}

void CBeefwebPlayer::SetSpeed(float speed)
{
  // The remote player has no notion of playback speed, so only the stopped,
  // paused and playing cases can be honoured.
  if (speed == 0.0f)
    m_client.Pause();
  else if (speed == 1.0f)
    m_client.Play();
  else
    return;

  m_speed = speed;
  CDataCacheCore::GetInstance().SetSpeed(1.0f, speed);
}

void CBeefwebPlayer::SetVolume(float volume)
{
  const BeefwebPlayerState state = m_listener->GetLastState();
  if (state.volumeMax <= state.volumeMin)
    return;

  // Kodi works in a 0..1 scale while the remote player reports a decibel range.
  const double db =
      state.volumeMin + static_cast<double>(volume) * (state.volumeMax - state.volumeMin);

  m_client.SetVolume(db);
}

void CBeefwebPlayer::SetMute(bool mute)
{
  m_client.SetMuted(mute);
}

void CBeefwebPlayer::GetAudioStreamInfo(int index, AudioStreamInfo& info) const
{
  BeefwebTrack track;
  {
    std::unique_lock<CCriticalSection> lock(m_stateSection);
    track = m_currentTrack;
  }

  if (track.path.empty())
    return;

  info.valid = true;
  info.codecName = track.codec;
  info.channels = track.channels;
  info.samplerate = track.sampleRate;

  // The remote player reports a rate in kbit/s while Kodi keeps bit/s.
  info.bitrate = track.bitrate * 1000;

  // Name the medium where it can be established, that being what someone
  // reading the display is more likely to recognise than the encoding inside
  // it. An SACD is direct stream digital held in a disc image; the extension
  // alone would say nothing, a video or data image carrying the same one.
  if (URIUtils::IsDiscImage(track.path) && IsDirectStreamDigital(track.codec))
    info.codecName = "SACD " + track.codec;
}

void CBeefwebPlayer::SetCurrentTrack(int index)
{
  {
    std::unique_lock<CCriticalSection> lock(m_stateSection);

    m_currentTrack = BeefwebTrack();
    for (const auto& track : m_loadedTracks)
    {
      if (track.index == index)
      {
        m_currentTrack = track;
        break;
      }
    }
  }

  // Kodi only asks for stream details when told they have changed.
  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
}

void CBeefwebPlayer::AnchorPlayTimes(const BeefwebPlayerState& state)
{
  {
    std::unique_lock<CCriticalSection> lock(m_timeSection);
    m_anchorPosition = state.position;
    m_anchorDuration = state.duration;
    m_anchorAt = std::chrono::steady_clock::now();
    m_anchorAdvancing = state.state == PlaybackState::PLAYING;
  }

  PublishPlayTimes();
}

void CBeefwebPlayer::PublishPlayTimes()
{
  double position = 0.0;
  double duration = 0.0;

  {
    std::unique_lock<CCriticalSection> lock(m_timeSection);

    position = m_anchorPosition;
    duration = m_anchorDuration;

    if (m_anchorAdvancing)
    {
      const std::chrono::duration<double> elapsed =
          std::chrono::steady_clock::now() - m_anchorAt;
      position += elapsed.count();
    }
  }

  // Carrying the position forward can overshoot when the track is nearly over
  // and the event announcing the next one has not arrived yet.
  if (duration > 0.0)
    position = std::min(position, duration);

  CDataCacheCore::GetInstance().SetPlayTimes(0, static_cast<int64_t>(position * SECONDS_TO_MS), 0,
                                             static_cast<int64_t>(duration * SECONDS_TO_MS));
}

void CBeefwebPlayer::NoteStopped(bool stopped)
{
  std::unique_lock<CCriticalSection> lock(m_timeSection);

  if (!stopped)
    m_stoppedAt.reset();
  else if (!m_stoppedAt.has_value())
    m_stoppedAt = std::chrono::steady_clock::now();
}

bool CBeefwebPlayer::HasStoppedForGood() const
{
  std::unique_lock<CCriticalSection> lock(m_timeSection);

  if (!m_stoppedAt.has_value())
    return false;

  return std::chrono::steady_clock::now() - *m_stoppedAt > STOP_GRACE;
}

bool CBeefwebPlayer::HasLostPlayer() const
{
  std::unique_lock<CCriticalSection> lock(m_timeSection);

  if (!m_disconnectedAt.has_value())
    return false;

  return std::chrono::steady_clock::now() - *m_disconnectedAt > RECONNECT_GRACE;
}

void CBeefwebPlayer::Process()
{
  while (!m_bStop)
  {
    PublishPlayTimes();

    // The remote player has been out of contact for long enough that it is not
    // coming back. Playback cannot be followed or controlled any more, so end
    // it rather than leave Kodi showing a track that may have stopped long ago.
    if (m_playing && HasLostPlayer() && m_playing.exchange(false))
    {
      CLog::LogF(LOGERROR, "gave up reaching the external player");
      m_speed = 0.0f;
      m_callback.OnPlayBackError();
    }

    // It has stayed stopped rather than moving on, so the queue really has
    // come to an end.
    if (m_playing && HasStoppedForGood() && m_playing.exchange(false))
    {
      CLog::LogF(LOGDEBUG, "external player reached the end of the queue");
      m_speed = 0.0f;
      m_callback.OnPlayBackStopped();
    }

    CThread::Sleep(PROGRESS_INTERVAL);
  }
}

void CBeefwebPlayer::OnBeefwebStateChanged(const BeefwebPlayerState& state)
{
  if (!m_playing)
    return;

  AnchorPlayTimes(state);

  // Anything but a stop means the remote player is alive and going, so a stop
  // noted a moment ago was only the gap between two tracks.
  if (state.state != PlaybackState::STOPPED)
    NoteStopped(false);

  PlaybackState previous;
  int previousIndex;
  {
    std::unique_lock<CCriticalSection> lock(m_stateSection);
    previous = m_lastState;
    previousIndex = m_lastItemIndex;
    m_lastState = state.state;
    m_lastItemIndex = state.itemIndex;
  }

  // One event covers everything the remote player does, so a change of track
  // has to be recognised by the position within the playlist having moved.
  // Once its playlist holds a whole album, which is what opening a disc image
  // or cue sheet gives it, the remote player walks through it by itself and
  // never reports stopping in between.
  if (state.state == PlaybackState::PLAYING && previousIndex >= 0 && state.itemIndex >= 0 &&
      state.itemIndex != previousIndex)
  {
    CLog::LogF(LOGDEBUG, "external player moved to position {}", state.itemIndex);

    SetCurrentTrack(state.itemIndex);

    // Show what is playing now. Kodi is not asked to move on, which would set
    // it choosing a next track of its own and undo the handover.
    for (const auto& track : m_loadedTracks)
    {
      if (track.index == state.itemIndex)
      {
        ReportTrack(track);
        break;
      }
    }

    return;
  }

  if (state.state == previous)
    return;

  switch (state.state)
  {
    case PlaybackState::PLAYING:
      if (previous == PlaybackState::PAUSED)
      {
        m_speed = 1.0f;
        CDataCacheCore::GetInstance().SetSpeed(1.0f, 1.0f);
        m_callback.OnPlayBackResumed();
      }
      break;

    case PlaybackState::PAUSED:
      m_speed = 0.0f;
      CDataCacheCore::GetInstance().SetSpeed(1.0f, 0.0f);
      m_callback.OnPlayBackPaused();
      break;

    case PlaybackState::STOPPED:
      // Only note the time. Whether this is the end of the queue or merely the
      // moment between two tracks cannot be told apart here, so it is left to
      // stand for a while before being reported. Doing it at once would tell
      // Kodi playback had finished just as the next track began.
      if (previous != PlaybackState::UNKNOWN)
        NoteStopped(true);
      break;

    case PlaybackState::UNKNOWN:
      break;
  }
}

void CBeefwebPlayer::OnBeefwebConnectionChanged(bool connected)
{
  std::unique_lock<CCriticalSection> lock(m_timeSection);

  if (connected)
  {
    m_disconnectedAt.reset();
    return;
  }

  // Losing the stream is not by itself a reason to stop: the remote player
  // carries on regardless, and the listener reconnects on its own. Only note
  // when contact was lost, so that a silence long enough to mean the player
  // really has gone can be told from a momentary drop.
  if (!m_disconnectedAt.has_value())
  {
    CLog::LogF(LOGWARNING, "lost contact with the external player, reconnecting");
    m_disconnectedAt = std::chrono::steady_clock::now();
  }
}
