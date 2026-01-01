#pragma once

// ============================================
// DEBUG: Set to 1 to disable debouncing delays for testing
// ============================================
#define CSPOT_DEBUG_NODELAY 1
// ============================================

#include <stdint.h>    // for uint32_t, uint8_t
#include <functional>  // for function
#include <memory>      // for shared_ptr, unique_ptr
#include <string>      // for string
#include <variant>     // for variant
#include <vector>      // for vector

#include "CDNAudioFile.h"  // for CDNTrackStream, CDNTrackStream::Track...
#include "TrackQueue.h"
#include "protobuf/spirc.pb.h"  // for MessageType

namespace cspot {
class TrackPlayer;
struct Context;

class SpircHandler {
 public:
  SpircHandler(std::shared_ptr<cspot::Context> ctx);

  enum class EventType {
    PLAY_PAUSE,
    VOLUME,
    TRACK_INFO,
    DISC,
    NEXT,
    PREV,
    SEEK,
    DEPLETED,
    FLUSH,
    PLAYBACK_START
  };

  typedef std::variant<TrackInfo, int, bool> EventData;

  struct Event {
    EventType eventType;
    EventData data;
  };

  typedef std::function<void(std::unique_ptr<Event>)> EventHandler;

  enum class NotifyType {
    STATE,   // 200ms delay - general state changes
    VOLUME   // 500ms delay - volume changes
  };

  void subscribeToMercury();
  std::shared_ptr<TrackPlayer> getTrackPlayer();

  void setEventHandler(EventHandler handler);

  void setPause(bool pause);

  bool previousSong();

  bool nextSong();

  void notifyAudioReachedPlayback();
  void notifyAudioEnded();
  void updatePositionMs(uint32_t position);
  void setRemoteVolume(int volume);
  void loadTrackFromURI(const std::string& uri);
  std::shared_ptr<cspot::TrackQueue> getTrackQueue() { return trackQueue; }

  void disconnect();
  void processDebouncing();  // Process pending debounced notifications

  // Debouncing delay constants (public for configuration checking)
#if CSPOT_DEBUG_NODELAY
  static constexpr uint32_t UPDATE_STATE_DELAY_MS = 0;     // DEBUG: No delay
  static constexpr uint32_t VOLUME_UPDATE_DELAY_MS = 0;    // DEBUG: No delay
#else
  static constexpr uint32_t UPDATE_STATE_DELAY_MS = 200;   // General state updates
  static constexpr uint32_t VOLUME_UPDATE_DELAY_MS = 500;  // Volume changes
#endif

 private:
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::TrackPlayer> trackPlayer;
  std::shared_ptr<cspot::TrackQueue> trackQueue;

  EventHandler eventHandler = nullptr;

  std::shared_ptr<cspot::PlaybackState> playbackState;

  // Debouncing state for protocol compliance (Phase 3)
  bool pendingNotify = false;
  bool pendingVolumeNotify = false;
  uint64_t lastNotifyRequestMs = 0;
  uint64_t lastVolumeNotifyRequestMs = 0;
  std::string notifyTriggerReason;
  std::string volumeTriggerReason;

  void sendCmd(MessageType typ, const std::string& triggerReason = "");

  void sendEvent(EventType type);
  void sendEvent(EventType type, EventData data);

  bool skipSong(TrackQueue::SkipDirection dir);
  void handleFrame(std::vector<uint8_t>& data);
  void notify(NotifyType type, const std::string& reason);
};
}  // namespace cspot
