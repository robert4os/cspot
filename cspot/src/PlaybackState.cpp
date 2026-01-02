#include "PlaybackState.h"

#include <string.h>  // for strdup, memcpy, strcpy, strlen
#include <cstdint>   // for uint8_t
#include <cstdlib>   // for free, NULL, realloc, rand
#include <cstring>
#include <memory>       // for shared_ptr
#include <type_traits>  // for remove_extent_t
#include <utility>      // for swap
#include <sstream>      // for stringstream
#include <iomanip>      // for setw, setfill, hex

#include "BellLogger.h"          // for AbstractLogger
#include "CSpotContext.h"        // for Context::ConfigState, Context (ptr o...
#include "ConstantParameters.h"  // for protocolVersion, swVersion
#include "Logger.h"              // for CSPOT_LOG
#include "NanoPBHelper.h"        // for pbEncode, pbPutString
#include "Packet.h"              // for cspot
#include "pb.h"                  // for pb_bytes_array_t, PB_BYTES_ARRAY_T_A...
#include "pb_decode.h"           // for pb_release
#include "protobuf/spirc.pb.h"

using namespace cspot;

PlaybackState::PlaybackState(std::shared_ptr<cspot::Context> ctx) {
  this->ctx = ctx;
  innerFrame = {};
  remoteFrame = {};

  // Prepare callbacks for decoding of remote frame track data
  remoteFrame.state.track.funcs.decode = &TrackReference::pbDecodeTrackList;
  remoteFrame.state.track.arg = &remoteTracks;

  innerFrame.ident = strdup(ctx->config.deviceId.c_str());
  innerFrame.protocol_version = strdup(protocolVersion);

  // Prepare default state
  innerFrame.state.has_position_ms = true;
  innerFrame.state.position_ms = 0;

  innerFrame.state.status = PlayStatus_kPlayStatusStop;
  innerFrame.state.has_status = true;

  innerFrame.state.position_measured_at = 0;
  innerFrame.state.has_position_measured_at = true;

  innerFrame.state.shuffle = false;
  innerFrame.state.has_shuffle = true;

  innerFrame.state.repeat = false;
  innerFrame.state.has_repeat = true;

  innerFrame.device_state.sw_version = strdup(swVersion);

  innerFrame.device_state.is_active = false;
  innerFrame.device_state.has_is_active = true;

  innerFrame.device_state.can_play = true;
  innerFrame.device_state.has_can_play = true;

  innerFrame.device_state.volume = ctx->config.volume;
  innerFrame.device_state.has_volume = true;

  innerFrame.device_state.name = strdup(ctx->config.deviceName.c_str());

  // Prepare player's capabilities (matching Bose Soundtouch that supports podcasts)
  addCapability(CapabilityType_kCanBePlayer, 1);
  addCapability(CapabilityType_kDeviceType, 4);  // 4 = SPEAKER
  addCapability(CapabilityType_kGaiaEqConnectId, 1);
  addCapability(CapabilityType_kSupportsLogout, 1);  // Changed to 1 like Bose
  addCapability(CapabilityType_kSupportsPlaylistV2, 1);
  addCapability(CapabilityType_kSupportsExternalEpisodes, 1);
  addCapability(CapabilityType_kIsObservable, 1);
  addCapability(CapabilityType_kVolumeSteps, 64);
  addCapability(CapabilityType_kCommandAcks, 1);  // Added - Bose has this
  addCapability(CapabilityType_kSupportedContexts, -1,
                std::vector<std::string>({"album", "playlist", "search",
                                          "inbox", "toplist", "starred",
                                          "publishedstarred", "track",
                                          "episode", "show"}));
  addCapability(CapabilityType_kSupportedTypes, -1,
                std::vector<std::string>({"audio/track", "audio/episode"}));
  
  // Automatically set capabilities_count to the actual number added
  innerFrame.device_state.capabilities_count = capabilityIndex;
  CSPOT_LOG(debug, "Initialized %d device capabilities", capabilityIndex);
}

PlaybackState::~PlaybackState() {
  pb_release(Frame_fields, &innerFrame);
  pb_release(Frame_fields, &remoteFrame);
}

void PlaybackState::setPlaybackState(const PlaybackState::State state) {
  switch (state) {
    case State::Loading:
      // Prepare the playback at position 0
      innerFrame.state.status = PlayStatus_kPlayStatusPause;
      innerFrame.state.position_ms = 0;
      innerFrame.state.position_measured_at =
          ctx->timeProvider->getSyncedTimestamp();
      break;
    case State::Playing:
      innerFrame.state.status = PlayStatus_kPlayStatusPlay;
      // Initialize position_measured_at if not set (first time playing)
      if (innerFrame.state.position_measured_at == 0) {
        innerFrame.state.position_measured_at = ctx->timeProvider->getSyncedTimestamp();
        CSPOT_LOG(info, "[POSITION] Initialized position_measured_at=%llu", 
                  innerFrame.state.position_measured_at);
      }
      // Don't reset position_measured_at here - let encodeCurrentFrame() update it
      // This allows position tracking to work correctly during continuous playback
      break;
    case State::Stopped:
      innerFrame.state.status = PlayStatus_kPlayStatusStop;
      break;
    case State::Paused:
      // Update state and recalculate current song position
      innerFrame.state.status = PlayStatus_kPlayStatusPause;
      uint64_t diff = ctx->timeProvider->getSyncedTimestamp() -
                      innerFrame.state.position_measured_at;
      this->updatePositionMs(innerFrame.state.position_ms + diff);
      break;
  }
}

void PlaybackState::syncWithRemote() {
  innerFrame.state.context_uri = (char*)realloc(
      innerFrame.state.context_uri, strlen(remoteFrame.state.context_uri) + 1);

  strcpy(innerFrame.state.context_uri, remoteFrame.state.context_uri);

  innerFrame.state.has_playing_track_index = true;
  innerFrame.state.playing_track_index = remoteFrame.state.playing_track_index;

  // Sync shuffle state from remote
  if (remoteFrame.state.has_shuffle) {
    setShuffle(remoteFrame.state.shuffle);
  }

  // Sync repeat state from remote
  if (remoteFrame.state.has_repeat) {
    setRepeat(remoteFrame.state.repeat);
  }
}

bool PlaybackState::isActive() {
  return innerFrame.device_state.is_active;
}

void PlaybackState::setActive(bool isActive) {
  innerFrame.device_state.is_active = isActive;
  if (isActive) {
    innerFrame.device_state.became_active_at =
        ctx->timeProvider->getSyncedTimestamp();
    innerFrame.device_state.has_became_active_at = true;
  }
}

void PlaybackState::updatePositionMs(uint32_t position) {
  uint32_t oldPosition = innerFrame.state.position_ms;
  uint64_t now = ctx->timeProvider->getSyncedTimestamp();
  uint64_t oldMeasuredAt = innerFrame.state.position_measured_at;
  
  // Always update both position and measured_at together
  // This ensures encodeCurrentFrame() extrapolation works correctly
  innerFrame.state.position_ms = position;
  innerFrame.state.position_measured_at = now;
  
  int32_t positionDiff = (int32_t)position - (int32_t)oldPosition;
  CSPOT_LOG(debug, "[POSITION] Updated: pos %u->%u (diff=%d ms), measured_at %llu->%llu",
            oldPosition, position, positionDiff, oldMeasuredAt, now);
}

void PlaybackState::setVolume(uint32_t volume) {
  innerFrame.device_state.volume = volume;
  ctx->config.volume = volume;
}

void PlaybackState::setShuffle(bool shuffle) {
  innerFrame.state.shuffle = shuffle;
  innerFrame.state.has_shuffle = true;
}

void PlaybackState::setRepeat(bool repeat) {
  innerFrame.state.repeat = repeat;
  innerFrame.state.has_repeat = true;
}

bool PlaybackState::decodeRemoteFrame(std::vector<uint8_t>& data) {
  pb_release(Frame_fields, &remoteFrame);

  remoteTracks.clear();

  pbDecode(remoteFrame, Frame_fields, data);

  return true;
}

std::vector<uint8_t> PlaybackState::encodeCurrentFrame(MessageType typ) {
  // Always update position_measured_at to NOW before sending, regardless of status
  // This prevents client-side prediction errors due to stale timestamps
  uint64_t now = ctx->timeProvider->getSyncedTimestamp();
  
  // Update position based on elapsed time if playing
  // This ensures Spotify UI shows correct position even without frequent SHADOW_TIME updates
  if (innerFrame.state.status == PlayStatus_kPlayStatusPlay) {
    uint64_t prev_measured = innerFrame.state.position_measured_at;
    uint32_t prev_pos = innerFrame.state.position_ms;
    uint32_t elapsed = now - innerFrame.state.position_measured_at;
    
    CSPOT_LOG(info, "Position update: now=%llu, prev_measured=%llu, prev_pos=%u, elapsed=%u",
              now, prev_measured, prev_pos, elapsed);
    
    // Only update if elapsed time is reasonable (< 60 seconds) to handle pauses
    // Anything over 60s is likely a timestamp error or very long pause
    if (elapsed < 60000) {
      innerFrame.state.position_ms += elapsed;
      innerFrame.state.position_measured_at = now;
      CSPOT_LOG(info, "Position updated: new_pos=%u, new_measured=%llu",
                innerFrame.state.position_ms, innerFrame.state.position_measured_at);
    } else {
      CSPOT_LOG(info, "Position update SKIPPED: elapsed=%u ms (>= 60000)", elapsed);
    }
  } else {
    // When paused/stopped, keep position but update measured_at to prevent stale timestamps
    innerFrame.state.position_measured_at = now;
    CSPOT_LOG(debug, "[ENCODE] Paused/Stopped: updated measured_at to %llu (pos=%u unchanged)",
              now, innerFrame.state.position_ms);
  }
  
  // Prepare current frame info
  innerFrame.version = 1;
  innerFrame.seq_nr = this->seqNum;
  innerFrame.typ = typ;
  innerFrame.state_update_id = ctx->timeProvider->getSyncedTimestamp();
  innerFrame.has_version = true;
  innerFrame.has_seq_nr = true;
  innerFrame.recipient_count = 0;
  innerFrame.has_state = true;
  innerFrame.has_device_state = true;
  innerFrame.has_typ = true;
  innerFrame.has_state_update_id = true;

  this->seqNum += 1;

  return pbEncode(Frame_fields, &innerFrame);
}

// Wraps messy nanopb setters. @TODO: find a better way to handle this
void PlaybackState::addCapability(CapabilityType typ, int intValue,
                                  std::vector<std::string> stringValue) {
  innerFrame.device_state.capabilities[capabilityIndex].has_typ = true;
  this->innerFrame.device_state.capabilities[capabilityIndex].typ = typ;

  if (intValue != -1) {
    this->innerFrame.device_state.capabilities[capabilityIndex].intValue[0] =
        intValue;
    this->innerFrame.device_state.capabilities[capabilityIndex].intValue_count =
        1;
  } else {
    this->innerFrame.device_state.capabilities[capabilityIndex].intValue_count =
        0;
  }

  for (int x = 0; x < stringValue.size(); x++) {
    pbPutString(stringValue[x],
                this->innerFrame.device_state.capabilities[capabilityIndex]
                    .stringValue[x]);
  }

  this->innerFrame.device_state.capabilities[capabilityIndex]
      .stringValue_count = stringValue.size();

  this->capabilityIndex += 1;
}

// Helper functions to convert enums to human-readable strings
static const char* messageTypeToString(MessageType typ) {
  switch (typ) {
    case MessageType_kMessageTypeHello: return "Hello";
    case MessageType_kMessageTypeGoodbye: return "Goodbye";
    case MessageType_kMessageTypeProbe: return "Probe";
    case MessageType_kMessageTypeNotify: return "Notify";
    case MessageType_kMessageTypeLoad: return "Load";
    case MessageType_kMessageTypePlay: return "Play";
    case MessageType_kMessageTypePause: return "Pause";
    case MessageType_kMessageTypePlayPause: return "PlayPause";
    case MessageType_kMessageTypeSeek: return "Seek";
    case MessageType_kMessageTypePrev: return "Prev";
    case MessageType_kMessageTypeNext: return "Next";
    case MessageType_kMessageTypeVolume: return "Volume";
    case MessageType_kMessageTypeShuffle: return "Shuffle";
    case MessageType_kMessageTypeRepeat: return "Repeat";
    case MessageType_kMessageTypeVolumeUp: return "VolumeUp";
    case MessageType_kMessageTypeVolumeDown: return "VolumeDown";
    case MessageType_kMessageTypeReplace: return "Replace";
    case MessageType_kMessageTypeLogout: return "Logout";
    case MessageType_kMessageTypeAction: return "Action";
    default: return "Unknown";
  }
}

static const char* capabilityTypeToString(CapabilityType typ) {
  switch (typ) {
    case CapabilityType_kSupportedContexts: return "SupportedContexts";
    case CapabilityType_kCanBePlayer: return "CanBePlayer";
    case CapabilityType_kRestrictToLocal: return "RestrictToLocal";
    case CapabilityType_kDeviceType: return "DeviceType";
    case CapabilityType_kGaiaEqConnectId: return "GaiaEqConnectId";
    case CapabilityType_kSupportsLogout: return "SupportsLogout";
    case CapabilityType_kIsObservable: return "IsObservable";
    case CapabilityType_kVolumeSteps: return "VolumeSteps";
    case CapabilityType_kSupportedTypes: return "SupportedTypes";
    case CapabilityType_kCommandAcks: return "CommandAcks";
    case CapabilityType_kSupportsRename: return "SupportsRename";
    case CapabilityType_kHidden: return "Hidden";
    case CapabilityType_kSupportsPlaylistV2: return "SupportsPlaylistV2";
    case CapabilityType_kSupportsExternalEpisodes: return "SupportsExternalEpisodes";
    default: return "Unknown";
  }
}

static const char* playStatusToString(PlayStatus status) {
  switch (status) {
    case PlayStatus_kPlayStatusStop: return "Stopped";
    case PlayStatus_kPlayStatusPlay: return "Playing";
    case PlayStatus_kPlayStatusPause: return "Paused";
    case PlayStatus_kPlayStatusLoading: return "Loading";
    default: return "Unknown";
  }
}

std::string PlaybackState::dumpFrameForDebug(MessageType typ, const std::string& triggerReason) {
  std::stringstream ss;
  
  ss << "\n========================================\n";
  ss << "SPIRC Frame Dump (OUTGOING)\n";
  ss << "========================================\n";
  if (!triggerReason.empty()) {
    ss << "\n--- Debug Info ---\n";
    ss << "Trigger Reason: " << triggerReason << "\n";
  }
  ss << "Message Type: " << messageTypeToString(typ) << " (" << typ << ")\n";
  ss << "Version: " << innerFrame.version << "\n";
  ss << "Sequence Number: " << innerFrame.seq_nr << "\n";
  ss << "Device ID: " << (innerFrame.ident ? innerFrame.ident : "NULL") << "\n";
  ss << "Protocol Version: " << (innerFrame.protocol_version ? innerFrame.protocol_version : "NULL") << "\n";
  ss << "State Update ID: " << innerFrame.state_update_id << "\n";
  
  ss << "\n--- Device State ---\n";
  ss << "Device Name: " << (innerFrame.device_state.name ? innerFrame.device_state.name : "NULL") << "\n";
  ss << "SW Version: " << (innerFrame.device_state.sw_version ? innerFrame.device_state.sw_version : "NULL") << "\n";
  ss << "Is Active: " << (innerFrame.device_state.is_active ? "true" : "false") << "\n";
  ss << "Can Play: " << (innerFrame.device_state.can_play ? "true" : "false") << "\n";
  ss << "Volume: " << innerFrame.device_state.volume << "\n";
  
  if (innerFrame.device_state.has_became_active_at) {
    ss << "Became Active At: " << innerFrame.device_state.became_active_at << "\n";
  }
  
  ss << "\n--- Capabilities (" << innerFrame.device_state.capabilities_count << " total) ---\n";
  
  // Check if capabilities_count matches actual capabilities added
  if (capabilityIndex != innerFrame.device_state.capabilities_count) {
    ss << "*** WARNING: capabilities_count=" << innerFrame.device_state.capabilities_count 
       << " but " << (int)capabilityIndex << " capabilities were added! ***\n";
    ss << "*** Missing capabilities will NOT be sent to Spotify! ***\n\n";
  }
  
  for (int i = 0; i < innerFrame.device_state.capabilities_count; i++) {
    auto& cap = innerFrame.device_state.capabilities[i];
    ss << "[" << i << "] " << capabilityTypeToString(cap.typ) << " (" << cap.typ << "): ";
    
    if (cap.intValue_count > 0) {
      ss << cap.intValue[0];
    }
    
    if (cap.stringValue_count > 0) {
      ss << "[";
      for (int j = 0; j < cap.stringValue_count; j++) {
        if (j > 0) ss << ", ";
        ss << "\"" << (cap.stringValue[j] ? cap.stringValue[j] : "NULL") << "\"";
      }
      ss << "]";
    }
    ss << "\n";
  }
  
  ss << "\n--- Playback State ---\n";
  if (innerFrame.state.has_status) {
    ss << "Status: " << playStatusToString(innerFrame.state.status) << " (" << innerFrame.state.status << ")\n";
  }
  if (innerFrame.state.has_position_ms) {
    ss << "Position: " << innerFrame.state.position_ms << " ms\n";
  }
  if (innerFrame.state.has_position_measured_at) {
    ss << "Position Measured At: " << innerFrame.state.position_measured_at << "\n";
  }
  if (innerFrame.state.has_shuffle) {
    ss << "Shuffle: " << (innerFrame.state.shuffle ? "true" : "false") << "\n";
  }
  if (innerFrame.state.has_repeat) {
    ss << "Repeat: " << (innerFrame.state.repeat ? "true" : "false") << "\n";
  }
  
  // Show playing track index for verification
  if (innerFrame.state.has_playing_track_index) {
    ss << "Playing Track Index: " << innerFrame.state.playing_track_index << " (reporting to Spotify)\n";
  }
  
  ss << "========================================\n";
  
  return ss.str();
}

std::string PlaybackState::dumpRemoteFrameForDebug() {
  std::stringstream output;
  
  output << "\n========================================\n";
  output << "SPIRC Frame Dump (INCOMING)\n";
  output << "========================================\n";
  
  // Message type
  const char* typeStr = "Unknown";
  switch (remoteFrame.typ) {
    case MessageType_kMessageTypeHello: typeStr = "Hello (1)"; break;
    case MessageType_kMessageTypeGoodbye: typeStr = "Goodbye (2)"; break;
    case MessageType_kMessageTypeProbe: typeStr = "Probe (3)"; break;
    case MessageType_kMessageTypeNotify: typeStr = "Notify (10)"; break;
    case MessageType_kMessageTypeLoad: typeStr = "Load (20)"; break;
    case MessageType_kMessageTypePlay: typeStr = "Play (21)"; break;
    case MessageType_kMessageTypePause: typeStr = "Pause (22)"; break;
    case MessageType_kMessageTypePlayPause: typeStr = "PlayPause (23)"; break;
    case MessageType_kMessageTypeSeek: typeStr = "Seek (24)"; break;
    case MessageType_kMessageTypePrev: typeStr = "Prev (25)"; break;
    case MessageType_kMessageTypeNext: typeStr = "Next (26)"; break;
    case MessageType_kMessageTypeVolume: typeStr = "Volume (27)"; break;
    case MessageType_kMessageTypeShuffle: typeStr = "Shuffle (28)"; break;
    case MessageType_kMessageTypeRepeat: typeStr = "Repeat (29)"; break;
    default: break;
  }
  output << "Message Type: " << typeStr << "\n";
  output << "Version: " << remoteFrame.version << "\n";
  output << "Sequence Number: " << remoteFrame.seq_nr << "\n";
  output << "Device ID: " << (remoteFrame.ident ? remoteFrame.ident : "NULL") << "\n";
  output << "Protocol Version: " << (remoteFrame.protocol_version ? remoteFrame.protocol_version : "NULL") << "\n";
  output << "State Update ID: " << remoteFrame.state_update_id << "\n";
  output << "Top-level Position (frame.position): " << remoteFrame.position << " ms\n";
  output << "Has State: " << (remoteFrame.has_state ? "yes" : "no") << "\n";
  
  // Recipients
  if (remoteFrame.recipient_count > 0) {
    output << "Recipients (" << remoteFrame.recipient_count << "): ";
    for (size_t i = 0; i < remoteFrame.recipient_count; i++) {
      if (i > 0) output << ", ";
      output << (remoteFrame.recipient[i] ? remoteFrame.recipient[i] : "NULL");
    }
    output << "\n";
  } else {
    output << "Recipients: (broadcast - no specific recipients)\n";
  }
  
  // Device state
  if (remoteFrame.has_device_state) {
    output << "\n--- Device State ---\n";
    output << "Device Name: " << (remoteFrame.device_state.name ? remoteFrame.device_state.name : "NULL") << "\n";
    output << "SW Version: " << (remoteFrame.device_state.sw_version ? remoteFrame.device_state.sw_version : "NULL") << "\n";
    output << "Is Active: " << (remoteFrame.device_state.is_active ? "true" : "false") << "\n";
    output << "Can Play: " << (remoteFrame.device_state.can_play ? "true" : "false") << "\n";
    output << "Volume: " << remoteFrame.device_state.volume << "\n";
    
    if (remoteFrame.device_state.has_became_active_at) {
      output << "Became Active At: " << remoteFrame.device_state.became_active_at << "\n";
    }
  }
  
  // Capabilities (skipped - C array structure)
  if (false && remoteFrame.has_device_state) {
    output << "\n--- Capabilities ---\n";
    // Commented out - capabilities is a C array, not a vector
    /*
    for (size_t i = 0; i < 17 && i < 20; i++) {
      const auto& cap = remoteFrame.device_state.capabilities[i];
      const char* capTypeStr = "Unknown";
      switch (cap.typ) {
        case CapabilityType_kSupportedContexts: capTypeStr = "SupportedContexts (1)"; break;
        case CapabilityType_kCanBePlayer: capTypeStr = "CanBePlayer (2)"; break;
        case CapabilityType_kRestrictToLocal: capTypeStr = "RestrictToLocal (3)"; break;
        case CapabilityType_kDeviceType: capTypeStr = "DeviceType (4)"; break;
        case CapabilityType_kGaiaEqConnectId: capTypeStr = "GaiaEqConnectId (5)"; break;
        case CapabilityType_kSupportsLogout: capTypeStr = "SupportsLogout (6)"; break;
        case CapabilityType_kIsObservable: capTypeStr = "IsObservable (7)"; break;
        case CapabilityType_kVolumeSteps: capTypeStr = "VolumeSteps (8)"; break;
        case CapabilityType_kSupportedTypes: capTypeStr = "SupportedTypes (9)"; break;
        case CapabilityType_kCommandAcks: capTypeStr = "CommandAcks (10)"; break;
        case CapabilityType_kSupportsRename: capTypeStr = "SupportsRename (11)"; break;
        case CapabilityType_kHidden: capTypeStr = "Hidden (12)"; break;
        case CapabilityType_kSupportsPlaylistV2: capTypeStr = "SupportsPlaylistV2 (13)"; break;
        case CapabilityType_kSupportsExternalEpisodes: capTypeStr = "SupportsExternalEpisodes (14)"; break;
        default: break;
      }
      output << "[" << i << "] " << capTypeStr << "\n";
    }
    */
  }
  
  // Playback state
  if (remoteFrame.has_state) {
    output << "\n--- Playback State ---\n";
    const char* statusStr = "Unknown";
    switch (remoteFrame.state.status) {
      case PlayStatus_kPlayStatusStop: statusStr = "Stopped (0)"; break;
      case PlayStatus_kPlayStatusPlay: statusStr = "Playing (1)"; break;
      case PlayStatus_kPlayStatusPause: statusStr = "Paused (2)"; break;
      case PlayStatus_kPlayStatusLoading: statusStr = "Loading (3)"; break;
      default: break;
    }
    output << "Status: " << statusStr << "\n";
    output << "Position: " << remoteFrame.state.position_ms << " ms";
    if (!remoteFrame.state.has_position_ms) {
      output << " (not set, using frame.position=" << remoteFrame.position << " ms)";
    }
    output << "\n";
    output << "Position Measured At: " << remoteFrame.state.position_measured_at << "\n";
    output << "Shuffle: " << (remoteFrame.state.shuffle ? "true" : "false") << "\n";
    output << "Repeat: " << (remoteFrame.state.repeat ? "true" : "false") << "\n";
    
    // Track queue information
    if (remoteFrame.state.has_playing_track_index) {
      output << "Playing Track Index: " << remoteFrame.state.playing_track_index << "\n";
    }
    
    // Show track queue if available
    if (!remoteTracks.empty()) {
      output << "\n--- Track Queue (" << remoteTracks.size() << " tracks) ---\n";
      for (size_t i = 0; i < remoteTracks.size() && i < 5; i++) {
        // Extract track identifier (GID or URI)
        std::string trackId = "unknown";
        if (remoteTracks[i].gid.size() > 0) {
          // Convert GID bytes to hex string (show first 12 chars)
          std::stringstream hexStream;
          size_t bytesToShow = std::min<size_t>(6, remoteTracks[i].gid.size());
          for (size_t j = 0; j < bytesToShow; j++) {
            hexStream << std::hex << std::setfill('0') << std::setw(2) 
                     << (int)(unsigned char)remoteTracks[i].gid[j];
          }
          trackId = hexStream.str();
        }
        
        // Mark current track
        const char* marker = "";
        if (remoteFrame.state.has_playing_track_index && 
            i == remoteFrame.state.playing_track_index) {
          marker = " ← CURRENT";
        }
        
        output << "[" << i << "] Track ID: " << trackId << marker << "\n";
      }
      if (remoteTracks.size() > 5) {
        output << "... (" << (remoteTracks.size() - 5) << " more tracks)\n";
      }
    }
  }
  
  output << "========================================\n";
  return output.str();
}
