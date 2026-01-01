#include "SpircHandler.h"

#include <cstdint>      // for uint8_t
#include <memory>       // for shared_ptr, make_unique, unique_ptr
#include <type_traits>  // for remove_extent_t
#include <utility>      // for move
#include <fstream>      // for debug file writing
#include <sstream>      // for stringstream
#include <iomanip>      // for hex formatting
#include <chrono>       // for timestamp
#include <ctime>        // for ctime

#include "BellLogger.h"      // for AbstractLogger
#include "CSpotContext.h"    // for Context::ConfigState, Context (ptr only)
#include "Logger.h"          // for CSPOT_LOG
#include "MercurySession.h"  // for MercurySession, MercurySession::Response
#include "NanoPBHelper.h"    // for pbDecode
#include "Packet.h"          // for cspot
#include "PlaybackState.h"   // for PlaybackState, PlaybackState::State
#include "TrackPlayer.h"     // for TrackPlayer
#include "TrackQueue.h"
#include "TrackReference.h"     // for TrackReference
#include "Utils.h"              // for stringHexToBytes
#include "pb_decode.h"          // for pb_release
#include "protobuf/spirc.pb.h"  // for Frame, State, Frame_fields, MessageTy...

using namespace cspot;

// Helper to get message type name for logging
static std::string getMessageTypeName(MessageType typ) {
  switch (typ) {
    case MessageType_kMessageTypeHello: return "Hello";
    case MessageType_kMessageTypeGoodbye: return "Goodbye";  
    case MessageType_kMessageTypeProbe: return "Probe";
    case MessageType_kMessageTypeNotify: return "Notify";
    case MessageType_kMessageTypeLoad: return "Load";
    case MessageType_kMessageTypePlay: return "Play";
    case MessageType_kMessageTypePause: return "Pause";
    case MessageType_kMessageTypeSeek: return "Seek";
    case MessageType_kMessageTypeNext: return "Next";
    case MessageType_kMessageTypePrev: return "Prev";
    case MessageType_kMessageTypeVolume: return "Volume";
    case MessageType_kMessageTypeShuffle: return "Shuffle";
    case MessageType_kMessageTypeRepeat: return "Repeat";
    case MessageType_kMessageTypeReplace: return "Replace";
    default: return "Unknown";
  }
}

SpircHandler::SpircHandler(std::shared_ptr<cspot::Context> ctx) {
  this->playbackState = std::make_shared<PlaybackState>(ctx);
  this->trackQueue = std::make_shared<cspot::TrackQueue>(ctx, playbackState);

  auto EOFCallback = [this]() {
    if (trackQueue->isFinished()) {
      CSPOT_LOG(info, "[EOF] Playlist depleted - no repeat");
      sendEvent(EventType::DEPLETED);
    } else {
      // Queue not finished (repeat is enabled), skip to next track which will loop to start
      CSPOT_LOG(info, "[EOF] Repeat enabled, skipping to next track (will loop)");
      trackQueue->skipTrack(TrackQueue::SkipDirection::NEXT, true);
    }
  };

  auto trackLoadedCallback = [this](std::shared_ptr<QueuedTrack> track,
                                    bool paused = false) {
    playbackState->setPlaybackState(paused ? PlaybackState::State::Paused
                                           : PlaybackState::State::Playing);
    playbackState->updatePositionMs(track->requestedPosition);

    // Don't notify here - status hasn't changed (still Playing/Paused)
    // notify() will be called by notifyAudioReachedPlayback() when renderer starts
    // The actual notify was deleted in this commenting commit 84b018df on Dec 31, 2025.

    // Send playback start event, pause/unpause per request
    sendEvent(EventType::PLAYBACK_START, (int)track->requestedPosition);
    sendEvent(EventType::PLAY_PAUSE, paused);
  };

  this->ctx = ctx;
  this->trackPlayer = std::make_shared<TrackPlayer>(
      ctx, trackQueue, EOFCallback, trackLoadedCallback);

  // Position updates are event-driven (trackLoaded, pause, seek, etc.)
  // No periodic callback needed - follows Spotify protocol (RH/librespot/07-STATE-SYNCHRONIZATION.md)

  // Subscribe to mercury on session ready
  ctx->session->setConnectedHandler([this]() { this->subscribeToMercury(); });
}

void SpircHandler::subscribeToMercury() {
  auto responseLambda = [this](MercurySession::Response& res) {
    if (res.fail)
      return;

    sendCmd(MessageType_kMessageTypeHello);
    CSPOT_LOG(debug, "Sent kMessageTypeHello!");

    // Assign country code
    this->ctx->config.countryCode = this->ctx->session->getCountryCode();
  };
  auto subscriptionLambda = [this](MercurySession::Response& res) {
    if (res.fail)
      return;
    CSPOT_LOG(debug, "Received subscription response");

    this->handleFrame(res.parts[0]);
  };

  ctx->session->executeSubscription(
      MercurySession::RequestType::SUB,
      "hm://remote/user/" + ctx->config.username + "/", responseLambda,
      subscriptionLambda);
}

void SpircHandler::loadTrackFromURI(const std::string& uri) {}

void SpircHandler::notifyAudioEnded() {
  playbackState->updatePositionMs(0);
  notify(NotifyType::STATE, "Track ended");
  trackPlayer->resetState(true);
}

void SpircHandler::notifyAudioReachedPlayback() {
  int offset = 0;

  // get HEAD track
  auto currentTrack = trackQueue->consumeTrack(nullptr, offset);

  // Do not execute when meta is already updated
  if (trackQueue->notifyPending) {
    trackQueue->notifyPending = false;

    playbackState->updatePositionMs(currentTrack->requestedPosition);

    // Reset position in queued track
    currentTrack->requestedPosition = 0;
  } else {
    trackQueue->skipTrack(TrackQueue::SkipDirection::NEXT, false);
    playbackState->updatePositionMs(0);

    // we moved to next track, re-acquire currentTrack again
    currentTrack = trackQueue->consumeTrack(nullptr, offset);
  }

  this->notify(NotifyType::STATE, "Track reached playback (decoder started)");

  sendEvent(EventType::TRACK_INFO, currentTrack->trackInfo);
}

void SpircHandler::updatePositionMs(uint32_t position) {
  // Get current state for drift detection
  uint32_t oldPosition = playbackState->innerFrame.state.position_ms;
  uint64_t oldMeasuredAt = playbackState->innerFrame.state.position_measured_at;
  uint64_t now = ctx->timeProvider->getSyncedTimestamp();
  
  // Calculate expected position based on time elapsed (client-side prediction)
  uint64_t elapsedMs = now - oldMeasuredAt;
  uint32_t expectedPosition = oldPosition + elapsedMs;
  
  // Only notify if position drift exceeds 100ms threshold (per Spotify protocol)
  int32_t drift = (int32_t)position - (int32_t)expectedPosition;
  bool significantDrift = std::abs(drift) > 100;
  
  playbackState->updatePositionMs(position);
  
  if (significantDrift) {
    CSPOT_LOG(debug, "[POSITION] Drift %d ms exceeds threshold, notifying", drift);
    notify(NotifyType::STATE, "Position drift correction");
  }
}

void SpircHandler::disconnect() {
  this->trackQueue->stopTask();
  this->trackPlayer->stop();
  this->ctx->session->disconnect();
}

void SpircHandler::handleFrame(std::vector<uint8_t>& data) {
  // Decode received spirc frame
  playbackState->decodeRemoteFrame(data);

  // Debug: Write incoming SPIRC frame details to file (only when CSPOT_DEBUG_FILES is set)
  if (getenv("CSPOT_DEBUG_FILES")) {
    std::stringstream ss;
    ss << "/tmp/spotupnp-device-spirc-" << ctx->config.deviceId.c_str() << ".log";
    std::string filename = ss.str();
    
    std::ofstream outFile(filename, std::ios::app);
    if (outFile.is_open()) {
      auto now = std::chrono::system_clock::now();
      time_t now_time = std::chrono::system_clock::to_time_t(now);
      
      outFile << "\n=== INCOMING FRAME ===" << std::ctime(&now_time);
      outFile << playbackState->dumpRemoteFrameForDebug();
      outFile << "Encoded Size: " << data.size() << " bytes\n";
      outFile << "\n";
      
      outFile.close();
      CSPOT_LOG(debug, "Incoming SPIRC frame appended to: %s", filename.c_str());
    }
  }

  // CRITICAL: Filter frames not addressed to us
  // SPIRC is a broadcast protocol - we receive ALL messages from ALL devices
  // Spotify uses 'recipient' field to target specific devices
  // We should only process:
  // 1. Commands explicitly addressed to us (recipient matches our device ID)
  // 2. Broadcast commands (no recipients specified) - these affect all devices
  // 3. Notify frames from other devices (for takeover detection)
  
  bool isAddressedToUs = false;
  bool isBroadcast = (playbackState->remoteFrame.recipient_count == 0);
  bool isNotify = (playbackState->remoteFrame.typ == MessageType_kMessageTypeNotify);
  bool isFromUs = (playbackState->remoteFrame.ident != nullptr && 
                   std::string(playbackState->remoteFrame.ident) == ctx->config.deviceId);
  
  // Never process our own frames (shouldn't happen, but be defensive)
  if (isFromUs) {
    CSPOT_LOG(debug, "Ignoring frame from ourselves");
    return;
  }
  
  // Check if this frame is addressed to us
  for (size_t i = 0; i < playbackState->remoteFrame.recipient_count; i++) {
    if (playbackState->remoteFrame.recipient[i] != nullptr &&
        std::string(playbackState->remoteFrame.recipient[i]) == ctx->config.deviceId) {
      isAddressedToUs = true;
      break;
    }
  }
  
  // Process frame only if:
  // - It's a Notify frame (needed for takeover detection), OR
  // - It's addressed to us, OR
  // - It's a broadcast (no recipients specified)
  if (!isNotify && !isAddressedToUs && !isBroadcast) {
    CSPOT_LOG(debug, "Ignoring %s command from device %s (addressed to %d other recipients)",
              getMessageTypeName(playbackState->remoteFrame.typ).c_str(),
              playbackState->remoteFrame.ident ? playbackState->remoteFrame.ident : "unknown",
              playbackState->remoteFrame.recipient_count);
    return;
  }

  switch (playbackState->remoteFrame.typ) {
    case MessageType_kMessageTypeNotify: {
      CSPOT_LOG(debug, "Notify frame");

      // Pause the playback if another player took control
      if (playbackState->isActive() &&
          playbackState->remoteFrame.device_state.is_active) {
        CSPOT_LOG(debug, "Another player took control, pausing playback");
        playbackState->setActive(false);

        this->trackPlayer->stop();
        sendEvent(EventType::DISC);
      }
      break;
    }
    case MessageType_kMessageTypeSeek: {
      // For Seek: frame.position is the TARGET, state.position_ms is the CURRENT position
      // Spotify quirk: Seek uses frame.position as target (opposite of Load!)
      uint32_t seekPosition = playbackState->remoteFrame.position;
      
      CSPOT_LOG(info, "[SEEK] Seeking from %u ms to %u ms (target=frame.position=%u)",
                playbackState->remoteFrame.state.position_ms,
                seekPosition,
                playbackState->remoteFrame.position);
      
      this->trackPlayer->seekMs(seekPosition);

      playbackState->updatePositionMs(seekPosition);

      notify(NotifyType::STATE, "Seek operation");

      sendEvent(EventType::SEEK, (int)seekPosition);
      break;
    }
    case MessageType_kMessageTypeVolume:
      playbackState->setVolume(playbackState->remoteFrame.volume);
      this->notify(NotifyType::VOLUME, "Volume frame from client");
      sendEvent(EventType::VOLUME, (int)playbackState->remoteFrame.volume);
      break;
    case MessageType_kMessageTypePause:
      // Don't sync position from remote - we are the authoritative source
      // Just pause at our current playback position
      playbackState->innerFrame.state.status = PlayStatus_kPlayStatusPause;
      notify(NotifyType::STATE, "Pause command from client");
      sendEvent(EventType::PLAY_PAUSE, true);
      break;
    case MessageType_kMessageTypePlay:
      setPause(false);
      break;
    case MessageType_kMessageTypeNext:
      if (nextSong()) {
        sendEvent(EventType::NEXT);
      }
      break;
    case MessageType_kMessageTypePrev:
      if (previousSong()) {
        sendEvent(EventType::PREV);
      }
      break;
    case MessageType_kMessageTypeLoad: {
      this->trackPlayer->start();

      CSPOT_LOG(debug, "Load frame %d!", playbackState->remoteTracks.size());
      CSPOT_LOG(info, "[LOAD] Position fields: frame.position=%u, frame.state.position_ms=%u, has_position_ms=%d",
               playbackState->remoteFrame.position,
               playbackState->remoteFrame.state.position_ms,
               playbackState->remoteFrame.state.has_position_ms);

      if (playbackState->remoteTracks.size() == 0) {
        CSPOT_LOG(info, "No tracks in frame, stopping playback");
        break;
      }

      playbackState->setActive(true);

      // For Load: state.position_ms is the TARGET position, frame.position is always 0
      // Spotify quirk: Load uses state.position_ms as target
      uint32_t startPosition = playbackState->remoteFrame.state.has_position_ms 
                               ? playbackState->remoteFrame.state.position_ms
                               : playbackState->remoteFrame.position;
      
      CSPOT_LOG(info, "[LOAD] Starting playback at position: %u ms (target=state.position_ms=%u, frame.position=%u)",
                startPosition,
                playbackState->remoteFrame.state.position_ms,
                playbackState->remoteFrame.position);
      
      playbackState->updatePositionMs(startPosition);
      playbackState->setPlaybackState(PlaybackState::State::Playing);

      playbackState->syncWithRemote();

      // Update track list with the same position
      trackQueue->updateTracks(startPosition, true);

      this->notify(NotifyType::STATE, "Load frame - new track queue");

      // Stop the current track, if any
      trackPlayer->resetState();
      break;
    }
    case MessageType_kMessageTypeReplace: {
      CSPOT_LOG(debug, "Got replace frame %d",
                playbackState->remoteTracks.size());
      playbackState->syncWithRemote();

      // 1st track is the current one, but update the position
      bool cleared = trackQueue->updateTracks(
          playbackState->remoteFrame.state.position_ms +
              ctx->timeProvider->getSyncedTimestamp() -
              playbackState->innerFrame.state.position_measured_at,
          false);

      this->notify(NotifyType::STATE, "Replace frame - queue updated");

      // need to re-load all if streaming track is completed
      if (cleared) {
        sendEvent(EventType::FLUSH);
        trackPlayer->resetState();
      }
      break;
    }
    case MessageType_kMessageTypeShuffle: {
      CSPOT_LOG(debug, "Got shuffle frame");

      // Update shuffle state from remote frame
      if (playbackState->remoteFrame.state.has_shuffle) {
        playbackState->setShuffle(playbackState->remoteFrame.state.shuffle);
      }

      this->notify(NotifyType::STATE, "Shuffle state change");
      break;
    }
    case MessageType_kMessageTypeRepeat: {
      CSPOT_LOG(debug, "Got repeat frame");

      // Update repeat state from remote frame
      if (playbackState->remoteFrame.state.has_repeat) {
        playbackState->setRepeat(playbackState->remoteFrame.state.repeat);
      }

      this->notify(NotifyType::STATE, "Repeat state change");
      break;
    }
    default:
      break;
  }
  
  // Debouncing is handled by periodic processDebouncing() call in main loop (spotify.cpp)
  // No need to call here - would cause timing issues with delay windows
}

void SpircHandler::setRemoteVolume(int volume) {
  // Spotify protocol uses u16 for volume (0-65535 valid range)
  if (volume < 0 || volume > 65535) {
    CSPOT_LOG(error, "[VOLUME] Invalid volume value: %d (must be 0-65535), ignoring", volume);
    return;
  }
  
  CSPOT_LOG(debug, "[VOLUME] setRemoteVolume called with volume=%d", volume);
  playbackState->setVolume(volume);
  notify(NotifyType::VOLUME, "Volume set remotely");
}

void SpircHandler::notify(NotifyType type, const std::string& reason) {
  // Runtime check for unexpected notify types
  if (type != NotifyType::STATE && type != NotifyType::VOLUME) {
    CSPOT_LOG(error, "[NOTIFY] Unexpected notify type: %d - defaulting to STATE", (int)type);
    type = NotifyType::STATE;
  }
  
  // NOTE: Both STATE and VOLUME use the same sendCmd(MessageType_kMessageTypeNotify) which sends
  // a full PlaybackState frame including position, status, AND volume. We don't support separate
  // volume-only frames yet. However, the debouncing mechanism fuses multiple updates that arrive
  // within the delay window into a single frame. The 500ms delay for VOLUME (vs 200ms for STATE)
  // provides more opportunity for fusion, reducing the total number of frames sent.
  
  if (type == NotifyType::VOLUME) {
    // Debounce volume updates (500ms delay per Spotify protocol)
    pendingVolumeNotify = true;
    lastVolumeNotifyRequestMs = ctx->timeProvider->getSyncedTimestamp();
    volumeTriggerReason = reason;
  } else { // NotifyType::STATE
    // Debounce state updates (200ms delay per Spotify protocol)
    pendingNotify = true;
    lastNotifyRequestMs = ctx->timeProvider->getSyncedTimestamp();
    notifyTriggerReason = reason;
  }
}

void SpircHandler::processDebouncing() {
  uint64_t now = ctx->timeProvider->getSyncedTimestamp();
  
  // Check if volume notification should be sent (500ms elapsed)
  if (pendingVolumeNotify && (now - lastVolumeNotifyRequestMs >= VOLUME_UPDATE_DELAY_MS)) {
    pendingVolumeNotify = false;
    this->sendCmd(MessageType_kMessageTypeNotify, volumeTriggerReason);
    CSPOT_LOG(debug, "[DEBOUNCE] VOLUME notify sent after %llu ms - %s", now - lastVolumeNotifyRequestMs, volumeTriggerReason.c_str());
    volumeTriggerReason.clear();
    return; // Only send one notify per cycle
  }
  
  // Check if general state notification should be sent (200ms elapsed)
  if (pendingNotify && (now - lastNotifyRequestMs >= UPDATE_STATE_DELAY_MS)) {
    pendingNotify = false;
    this->sendCmd(MessageType_kMessageTypeNotify, notifyTriggerReason);
    CSPOT_LOG(debug, "[DEBOUNCE] STATE notify sent after %llu ms - %s", now - lastNotifyRequestMs, notifyTriggerReason.c_str());
    notifyTriggerReason.clear();
  }
}

bool SpircHandler::skipSong(TrackQueue::SkipDirection dir) {
  bool skipped = trackQueue->skipTrack(dir);

  // Reset track state
  trackPlayer->resetState(!skipped);

  // send NEXT or PREV event only when successful
  return skipped;
}

bool SpircHandler::nextSong() {
  return skipSong(TrackQueue::SkipDirection::NEXT);
}

bool SpircHandler::previousSong() {
  return skipSong(TrackQueue::SkipDirection::PREV);
}

std::shared_ptr<TrackPlayer> SpircHandler::getTrackPlayer() {
  return this->trackPlayer;
}

void SpircHandler::sendCmd(MessageType typ, const std::string& triggerReason) {
  // Serialize current player state
  auto encodedFrame = playbackState->encodeCurrentFrame(typ);
  
  if (typ == MessageType_kMessageTypeHello) {
    CSPOT_LOG(debug, "Sending SPIRC Hello frame with device capabilities");
  }

  // Debug: Write SPIRC frame details to file (only when CSPOT_DEBUG_FILES is set)
  if (getenv("CSPOT_DEBUG_FILES")) {
    std::stringstream ss;
    ss << "/tmp/spotupnp-device-spirc-" << ctx->config.deviceId.c_str() << ".log";
    std::string filename = ss.str();
    
    std::ofstream outFile(filename, std::ios::app);
    if (outFile.is_open()) {
      auto now = std::chrono::system_clock::now();
      time_t now_time = std::chrono::system_clock::to_time_t(now);
      
      outFile << "\n=== OUTGOING FRAME ===" << std::ctime(&now_time);
      outFile << playbackState->dumpFrameForDebug(typ, triggerReason);
      outFile << "Encoded Size: " << encodedFrame.size() << " bytes\n";
      outFile << "\n";
      
      outFile.close();
      CSPOT_LOG(debug, "SPIRC frame appended to: %s", filename.c_str());
    }
  }

  auto responseLambda = [=](MercurySession::Response& res) {
  };
  auto parts = MercurySession::DataParts({encodedFrame});
  ctx->session->execute(MercurySession::RequestType::SEND,
                        "hm://remote/user/" + ctx->config.username + "/",
                        responseLambda, parts);
}
void SpircHandler::setEventHandler(EventHandler handler) {
  this->eventHandler = handler;
}

void SpircHandler::setPause(bool isPaused) {
  if (isPaused) {
    CSPOT_LOG(debug, "External pause command");
    playbackState->setPlaybackState(PlaybackState::State::Paused);
  } else {
    CSPOT_LOG(debug, "External play command");

    playbackState->setPlaybackState(PlaybackState::State::Playing);
    
    // Recalibrate playback timer to exclude paused time
    trackPlayer->recalibrateTimer();
  }
  notify(NotifyType::STATE, isPaused ? "Paused by application" : "Resumed by application");
  sendEvent(EventType::PLAY_PAUSE, isPaused);
}

void SpircHandler::sendEvent(EventType type) {
  auto event = std::make_unique<Event>();
  event->eventType = type;
  event->data = {};
  eventHandler(std::move(event));
}

void SpircHandler::sendEvent(EventType type, EventData data) {
  auto event = std::make_unique<Event>();
  event->eventType = type;
  event->data = data;
  eventHandler(std::move(event));
}
