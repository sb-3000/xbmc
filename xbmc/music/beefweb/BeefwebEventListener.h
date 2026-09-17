/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BeefwebClient.h"
#include "threads/CriticalSection.h"
#include "threads/Thread.h"

#include <atomic>
#include <memory>
#include <string>

namespace XFILE
{
class CCurlFile;
}

namespace KODI::MUSIC::BEEFWEB
{

/*! \brief Receives state changes pushed by the remote player. */
class IBeefwebEventHandler
{
public:
  virtual ~IBeefwebEventHandler() = default;

  /*! \brief Called when the remote player reports new state.
   *  \remark Invoked on the listener thread, not the main thread.
   */
  virtual void OnBeefwebStateChanged(const BeefwebPlayerState& state) = 0;

  /*! \brief Called when the connection to the remote player is established or lost.
   *  \remark Invoked on the listener thread, not the main thread.
   */
  virtual void OnBeefwebConnectionChanged(bool connected) = 0;
};

/*!
 * \brief Holds a server-sent events stream open against the remote player and
 *        publishes the state it pushes.
 *
 * The remote player sends a complete state snapshot on connect and then again
 * whenever something changes, so this doubles as the initial state fetch and
 * removes any need to poll. Idle keep-alives arrive roughly every 15 seconds
 * and are discarded.
 *
 * The stream is reconnected with a backoff whenever it drops, so the listener
 * survives the remote player being closed and reopened.
 */
class CBeefwebEventListener : public CThread
{
public:
  explicit CBeefwebEventListener(IBeefwebEventHandler* handler = nullptr);
  ~CBeefwebEventListener() override;

  /*! \brief Start listening to the given server, replacing any current one. */
  void Start(const std::string& host, int port);

  /*! \brief Stop listening and wait for the thread to finish. */
  void Stop();

  bool IsConnected() const { return m_connected; }

  /*! \brief The most recent state pushed by the remote player. */
  BeefwebPlayerState GetLastState() const;

protected:
  void Process() override;

private:
  /*! \brief Run one connection until it drops. \return true if it connected. */
  bool RunStream();

  /*! \brief Handle one line read from the stream. */
  void HandleLine(const std::string& line);

  /*! \brief Handle one "data:" payload from the stream. */
  void HandlePayload(const std::string& payload);

  void SetConnected(bool connected);

  IBeefwebEventHandler* const m_handler{nullptr};

  std::string m_host;
  int m_port{0};

  mutable CCriticalSection m_stateSection;
  BeefwebPlayerState m_state;

  std::atomic<bool> m_connected{false};

  /*! \brief Guards m_curl so that Stop() can cancel a blocked read.
   *
   * Held only while reading or replacing the pointer, never while cancelling:
   * cancelling blocks until the transfer closes, which the listener thread
   * cannot do while this lock is taken.
   */
  CCriticalSection m_curlSection;

  /*! \brief The transfer in progress, shared so that a caller cancelling it
   *         keeps it alive for the duration of the call.
   */
  std::shared_ptr<XFILE::CCurlFile> m_curl;
};

} // namespace KODI::MUSIC::BEEFWEB
