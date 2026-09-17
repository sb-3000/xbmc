/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BeefwebEventListener.h"

#include "URL.h"
#include "filesystem/CurlFile.h"
#include "utils/JSONVariantParser.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

using namespace KODI::MUSIC::BEEFWEB;
using namespace std::chrono_literals;

namespace
{
constexpr int CONNECT_TIMEOUT = 5;

// Curl gives up on a transfer whose rate stays under one byte per second for
// this long. The remote player sends nothing between events but a keep-alive
// of a few bytes every 15 seconds, which averages below that floor for as long
// as a track plays, so any ordinary value here would drop a perfectly healthy
// stream on a timer. The rate test is therefore put out of reach and a dead
// connection is left to surface as a failed read instead.
constexpr int LOW_SPEED_TIME = 24 * 60 * 60;

constexpr auto DATA_PREFIX = "data:";

// Size of a single read from the stream. Reads return as soon as anything has
// arrived, so this only bounds how much is collected at once.
constexpr size_t READ_CHUNK = 4096;

// Cap on a single unterminated line, so that a server that never sends a
// newline cannot grow the pending buffer without limit. A state payload with
// long track titles is well under a kilobyte.
constexpr size_t MAX_PENDING = 1024 * 1024;

constexpr auto RECONNECT_DELAY_MIN = 1s;
constexpr auto RECONNECT_DELAY_MAX = 30s;
} // unnamed namespace

CBeefwebEventListener::CBeefwebEventListener(IBeefwebEventHandler* handler)
  : CThread("BeefwebEvents"), m_handler(handler)
{
}

CBeefwebEventListener::~CBeefwebEventListener()
{
  Stop();
}

void CBeefwebEventListener::Start(const std::string& host, int port)
{
  // Already following this server, so leave the stream alone. Tearing it down
  // and building it up again for every track would report the connection as
  // lost each time, which to anything watching is indistinguishable from the
  // remote player having gone away.
  if (IsRunning() && m_host == host && m_port == port)
    return;

  Stop();

  if (host.empty() || port <= 0)
  {
    CLog::LogF(LOGERROR, "refusing to start without a server address");
    return;
  }

  m_host = host;
  m_port = port;

  Create();
}

void CBeefwebEventListener::Stop()
{
  m_bStop = true;

  // The thread is normally blocked reading the stream, which only returns when
  // the remote player sends something. Cancelling makes the read give up.
  //
  // Take a reference to the transfer and release the lock before cancelling.
  // Cancelling spins until the transfer has actually been closed, and the
  // listener thread needs this same lock on its way to closing it, so holding
  // the lock across the call would deadlock the two threads against each
  // other. The shared reference keeps the transfer alive meanwhile.
  std::shared_ptr<XFILE::CCurlFile> curl;
  {
    std::unique_lock<CCriticalSection> lock(m_curlSection);
    curl = m_curl;
  }

  if (curl)
    curl->Cancel();

  // Safe even if the thread was never created: CThread's start event is
  // constructed already signalled, and joining a thread that exited on its own
  // is what releases it.
  StopThread(true);

  SetConnected(false);
}

BeefwebPlayerState CBeefwebEventListener::GetLastState() const
{
  std::unique_lock<CCriticalSection> lock(m_stateSection);
  return m_state;
}

void CBeefwebEventListener::Process()
{
  auto delay = RECONNECT_DELAY_MIN;

  while (!m_bStop)
  {
    if (RunStream())
    {
      // The connection worked at least once, so treat the next drop as a
      // transient failure rather than backing off from where we left off.
      delay = RECONNECT_DELAY_MIN;
    }

    SetConnected(false);

    if (m_bStop)
      break;

    Sleep(delay);

    delay = std::min(delay * 2, RECONNECT_DELAY_MAX);
  }

  SetConnected(false);
}

bool CBeefwebEventListener::RunStream()
{
  const std::string url =
      StringUtils::Format("http://{}:{}/api/query/updates?player=true", m_host, m_port);

  auto curl = std::make_shared<XFILE::CCurlFile>();
  curl->SetTimeout(CONNECT_TIMEOUT);
  curl->SetLowSpeedTime(LOW_SPEED_TIME);
  curl->SetRequestHeader("Accept", "text/event-stream");

  {
    std::unique_lock<CCriticalSection> lock(m_curlSection);
    if (m_bStop)
      return false;
    m_curl = curl;
  }

  bool connected = false;

  if (curl->Open(CURL(url)))
  {
    connected = true;
    SetConnected(true);
    CLog::LogF(LOGDEBUG, "listening to {}:{}", m_host, m_port);

    // Read raw rather than by line: a line read asks the transfer to buffer as
    // many bytes as the line buffer holds before it returns, which for a feed
    // that trickles a few hundred bytes at a time would stall for hours. A
    // plain read returns as soon as anything arrives, so lines are assembled
    // here instead.
    std::vector<char> chunk(READ_CHUNK);
    std::string pending;

    while (!m_bStop)
    {
      const ssize_t bytes = curl->Read(chunk.data(), chunk.size());
      if (bytes <= 0)
        break; // cancelled, errored, or the stream ended

      pending.append(chunk.data(), static_cast<size_t>(bytes));

      size_t newline;
      while ((newline = pending.find('\n')) != std::string::npos)
      {
        HandleLine(pending.substr(0, newline));
        pending.erase(0, newline + 1);
      }

      if (pending.size() > MAX_PENDING)
      {
        CLog::LogF(LOGWARNING, "discarding an overlong line from the event stream");
        pending.clear();
      }
    }

    if (!m_bStop)
      CLog::LogF(LOGDEBUG, "stream from {}:{} ended", m_host, m_port);
  }
  else if (!m_bStop)
  {
    CLog::LogF(LOGDEBUG, "cannot reach {}:{}", m_host, m_port);
  }

  // Close before dropping the reference rather than leaving it to the
  // destructor: a caller blocked in Cancel() is waiting for exactly this, and
  // it still holds a reference of its own so the destructor would not run.
  curl->Close();

  {
    std::unique_lock<CCriticalSection> lock(m_curlSection);
    m_curl.reset();
  }

  return connected;
}

void CBeefwebEventListener::HandleLine(const std::string& line)
{
  std::string text(line);
  StringUtils::Trim(text);

  if (!StringUtils::StartsWith(text, DATA_PREFIX))
    return; // blank separator line or comment

  std::string payload = text.substr(std::char_traits<char>::length(DATA_PREFIX));
  StringUtils::Trim(payload);

  HandlePayload(payload);
}

void CBeefwebEventListener::HandlePayload(const std::string& payload)
{
  if (payload.empty())
    return;

  CVariant event;
  if (!CJSONVariantParser::Parse(payload, event))
  {
    CLog::LogF(LOGWARNING, "ignoring malformed event payload");
    return;
  }

  // Keep-alives arrive as an empty object and carry no state.
  if (!event.isMember("player"))
    return;

  BeefwebPlayerState state;
  if (!CBeefwebClient::ParsePlayerState(event["player"], state))
    return;

  {
    std::unique_lock<CCriticalSection> lock(m_stateSection);
    m_state = state;
  }

  if (m_handler)
    m_handler->OnBeefwebStateChanged(state);
}

void CBeefwebEventListener::SetConnected(bool connected)
{
  if (m_connected.exchange(connected) == connected)
    return;

  if (m_handler)
    m_handler->OnBeefwebConnectionChanged(connected);
}
