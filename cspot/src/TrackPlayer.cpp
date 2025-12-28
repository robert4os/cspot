#include "TrackPlayer.h"

#include <chrono>       // for steady_clock, duration_cast
#include <mutex>        // for mutex, scoped_lock
#include <string>       // for string
#include <type_traits>  // for remove_extent_t
#include <vector>       // for vector, vector<>::value_type

#include "BellLogger.h"        // for AbstractLogger
#include "BellUtils.h"         // for BELL_SLEEP_MS
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "TrackQueue.h"        // for CDNTrackStream, CDNTrackStream::TrackInfo
#include "WrappedSemaphore.h"  // for WrappedSemaphore

#ifdef BELL_VORBIS_FLOAT
#define VORBIS_SEEK(file, position) \
  (ov_time_seek(file, (double)position / 1000))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, 0, 2, 1, section))
#else
#define VORBIS_SEEK(file, position) (ov_time_seek(file, position))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, section))
#endif

namespace cspot {
struct Context;
struct TrackReference;
}  // namespace cspot

using namespace cspot;

static size_t vorbisReadCb(void* ptr, size_t size, size_t nmemb,
                           TrackPlayer* self) {
  return self->_vorbisRead(ptr, size, nmemb);
}

static int vorbisCloseCb(TrackPlayer* self) {
  return self->_vorbisClose();
}

static int vorbisSeekCb(TrackPlayer* self, int64_t offset, int whence) {

  return self->_vorbisSeek(offset, whence);
}

static long vorbisTellCb(TrackPlayer* self) {
  return self->_vorbisTell();
}

TrackPlayer::TrackPlayer(std::shared_ptr<cspot::Context> ctx,
                         std::shared_ptr<cspot::TrackQueue> trackQueue,
                         EOFCallback eof, TrackLoadedCallback trackLoaded)
    : bell::Task("cspot_player", 48 * 1024, 5, 1) {
  this->ctx = ctx;
  this->eofCallback = eof;
  this->trackLoaded = trackLoaded;
  this->trackQueue = trackQueue;
  this->playbackSemaphore = std::make_unique<bell::WrappedSemaphore>(5);

  // Initialize vorbis callbacks
  vorbisFile = {};
  vorbisCallbacks = {
      (decltype(ov_callbacks::read_func))&vorbisReadCb,
      (decltype(ov_callbacks::seek_func))&vorbisSeekCb,
      (decltype(ov_callbacks::close_func))&vorbisCloseCb,
      (decltype(ov_callbacks::tell_func))&vorbisTellCb,
  };
}

TrackPlayer::~TrackPlayer() {
  isRunning = false;
  resetState();
  std::scoped_lock lock(runningMutex);
}

void TrackPlayer::start() {
  if (!isRunning) {
    isRunning = true;
    startTask();
  }
}

void TrackPlayer::stop() {
  isRunning = false;
  resetState();
  std::scoped_lock lock(runningMutex);
}

void TrackPlayer::resetState(bool paused) {
  // Mark for reset
  this->pendingReset = true;
  this->currentSongPlaying = false;
  this->startPaused = paused;

  std::scoped_lock lock(dataOutMutex);

  CSPOT_LOG(info, "Resetting state");
}

void TrackPlayer::seekMs(size_t ms) {
  if (inFuture) {
    // We're in the middle of the next track, so we need to reset the player in order to seek
    resetState();
  }

  CSPOT_LOG(info, "Seeking...");
  this->pendingSeekPositionMs = ms;
}

void TrackPlayer::runTask() {
  std::scoped_lock lock(runningMutex);

  std::shared_ptr<QueuedTrack> track, newTrack = nullptr;

  int trackOffset = 0;
  bool eof = false;
  bool endOfQueueReached = false;

  while (isRunning) {
    // Ensure we even have any tracks to play
    if (!this->trackQueue->hasTracks() ||
        (!pendingReset && endOfQueueReached && trackQueue->isFinished())) {
      this->trackQueue->playableSemaphore->twait(300);
      continue;
    }

    // Last track was interrupted, reset to default
    if (pendingReset) {
      track = nullptr;
      pendingReset = false;
      inFuture = false;
    }

    endOfQueueReached = false;

    // Wait 800ms. If next reset is requested in meantime, restart the queue.
    // Gets rid of excess actions during rapid queueing
    BELL_SLEEP_MS(50);

    if (pendingReset) {
      continue;
    }

    newTrack = trackQueue->consumeTrack(track, trackOffset);

    if (newTrack == nullptr) {
      if (trackOffset == -1) {
        // Reset required
        track = nullptr;
      }

      BELL_SLEEP_MS(100);
      continue;
    }

    track = newTrack;

    inFuture = trackOffset > 0;

    if (track->state != QueuedTrack::State::READY) {
      track->loadedSemaphore->twait(5000);

      if (track->state != QueuedTrack::State::READY) {
        CSPOT_LOG(error, "Track failed to load, skipping it (httpStatus=%d, retryAfter=%d)", 
                  track->httpStatusCode, track->retryAfterSeconds);
        
        consecutiveFailures++;
        
        // Check if we should stop retrying based on error type and count
        const int MAX_RETRIES = 12;
        const int MAX_RATE_LIMIT_RETRIES = 3;
        const int MAX_AUTH_RETRIES = 1;
        
        bool shouldGiveUp = false;
        std::string giveUpReason = "";
        
        // Auth errors (401, 403) - give up immediately
        if (track->httpStatusCode == 401 || track->httpStatusCode == 403) {
          if (consecutiveFailures >= MAX_AUTH_RETRIES) {
            shouldGiveUp = true;
            giveUpReason = "authentication/authorization failure";
          }
        }
        // Rate limiting (429) - give up after a few tries
        else if (track->httpStatusCode == 429) {
          if (consecutiveFailures >= MAX_RATE_LIMIT_RETRIES) {
            shouldGiveUp = true;
            giveUpReason = "persistent rate limiting";
          }
        }
        // General failure limit
        else if (consecutiveFailures >= MAX_RETRIES) {
          shouldGiveUp = true;
          giveUpReason = "too many consecutive failures";
        }
        
        if (shouldGiveUp) {
          CSPOT_LOG(error, "Giving up after %d failures (%s), stopping playback", 
                   consecutiveFailures.load(), giveUpReason.c_str());
          consecutiveFailures = 0;  // Reset for next playback session
          // Don't call eofCallback - just stop trying
          this->currentSongPlaying = false;
          continue;
        }
        
        // Apply delay BEFORE triggering skip to prevent TrackQueue from loading next track
        int delayMs = 0;
        
        // Use server-provided Retry-After if available, otherwise exponential backoff
        if (track->retryAfterSeconds > 0) {
          delayMs = track->retryAfterSeconds * 1000;
          CSPOT_LOG(info, "Rate limiting: waiting %d seconds as requested by server (failure #%d/%d)", 
                    track->retryAfterSeconds, consecutiveFailures.load(), 
                    track->httpStatusCode == 429 ? MAX_RATE_LIMIT_RETRIES : MAX_RETRIES);
          BELL_SLEEP_MS(delayMs);
          CSPOT_LOG(info, "Rate limiting: wait complete, resuming");
        } else if (consecutiveFailures > 1) {
          // Delay: 1s, 2s, 4s, 8s, max 30s
          delayMs = std::min(1000 * (1 << (consecutiveFailures - 2)), 30000);
          CSPOT_LOG(info, "Rate limiting: exponential backoff %d ms (failure #%d/%d)", 
                    delayMs, consecutiveFailures.load(), MAX_RETRIES);
          BELL_SLEEP_MS(delayMs);
          CSPOT_LOG(info, "Rate limiting: backoff complete, resuming");
        }
        
        // Now trigger skip AFTER delay
        this->eofCallback();
        
        continue;
      }
    }

    // Track loaded successfully - reset failure counter
    consecutiveFailures = 0;

    CSPOT_LOG(info, "Got track ID=%s", track->identifier.c_str());

    currentSongPlaying = true;

    {
      std::scoped_lock lock(playbackMutex);

      currentTrackStream = track->getAudioFile();

      // Open the stream
      currentTrackStream->openStream();

      if (pendingReset || !currentSongPlaying) {
        continue;
      }

      if (trackOffset == 0 && pendingSeekPositionMs == 0) {
        // Task is stopped before callbacks cleared (see CSpotPlayer destructor)
        if (this->trackLoaded) {
          this->trackLoaded(track, startPaused);
        }
        startPaused = false;
      }

      int32_t r =
          ov_open_callbacks(this, &vorbisFile, NULL, 0, vorbisCallbacks);

#ifdef BELL_VORBIS_FLOAT
      double vorbis_duration_sec = ov_time_total(&vorbisFile, -1);
      int64_t vorbis_duration_ms = (int64_t)(vorbis_duration_sec * 1000.0);
#else
      int64_t vorbis_duration_ms = ov_time_total(&vorbisFile, -1);
#endif
      
      vorbis_info* vi = ov_info(&vorbisFile, -1);
      CSPOT_LOG(debug, "[VORBIS] Opened file, CDN size=%zu bytes, vorbis reports duration=%lld ms",
               currentTrackStream->getSize(), (long long)vorbis_duration_ms);
      CSPOT_LOG(debug, "[VORBIS] Stream info: rate=%ld Hz, channels=%d, bitrate_nominal=%ld",
               vi->rate, vi->channels, vi->bitrate_nominal);

      if (pendingSeekPositionMs > 0) {
        track->requestedPosition = pendingSeekPositionMs;
      }

      if (track->requestedPosition > 0) {
        VORBIS_SEEK(&vorbisFile, track->requestedPosition);
      }

      eof = false;
      track->loading = true;

      CSPOT_LOG(info, "Playing");

      // Rate limiting: track playback start time and bytes sent
      auto playbackStartTime = std::chrono::steady_clock::now();
      size_t totalPCMBytes = 0;
      size_t totalSleptMs = 0;
      const size_t bytesPerSecond = vi->rate * vi->channels * 2;  // 44100 * 2 * 2 = 176400 bytes/sec
      int64_t lastLogMs = 0;

      while (!eof && currentSongPlaying) {
        // Execute seek if needed
        if (pendingSeekPositionMs > 0) {
          uint32_t seekPosition = pendingSeekPositionMs;

          // Reset the pending seek position
          pendingSeekPositionMs = 0;

          // Seek to the new position
          VORBIS_SEEK(&vorbisFile, seekPosition);
        }

        long ret = VORBIS_READ(&vorbisFile, (char*)&pcmBuffer[0],
                               pcmBuffer.size(), &currentSection);

        if (ret == 0) {
          auto elapsed = std::chrono::steady_clock::now() - playbackStartTime;
          auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
          auto expectedMs = (totalPCMBytes * 1000) / bytesPerSecond;
          CSPOT_LOG(info, "EOF - expected time: %.1fs, decoded PCM: %zu bytes", expectedMs/1000.0, totalPCMBytes);
          // and done :)
          eof = true;
        } else if (ret < 0) {
          CSPOT_LOG(error, "An error has occured in the stream %d", ret);
          currentSongPlaying = false;
        } else {
          // Track decoded PCM for rate limiting
          totalPCMBytes += ret;
          
          if (this->dataCallback) {
            auto toWrite = ret;

            while (!eof && currentSongPlaying && !pendingReset && toWrite > 0) {
              int written = 0;
              {
                std::scoped_lock dataOutLock(dataOutMutex);
                // If reset happened during playback, return
                if (!currentSongPlaying || pendingReset)
                  break;

                // Task is stopped before callback cleared (see CSpotPlayer destructor)
                if (!this->dataCallback)
                  break;

                written = this->dataCallback(pcmBuffer.data() + (ret - toWrite),
                                             toWrite, track->identifier);
              }
              if (written == 0) {
                BELL_SLEEP_MS(50);
              }
              toWrite -= written;
            }
          }
          
          // Rate limiting: ensure we don't decode faster than real-time playback
          // Do this AFTER dataCallback to account for callback blocking time
          auto elapsed = std::chrono::steady_clock::now() - playbackStartTime;
          auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
          auto expectedMs = (totalPCMBytes * 1000) / bytesPerSecond;
          
          // If we've decoded ahead of real-time, sleep to stay synchronized
          if (expectedMs > elapsedMs) {
            int64_t sleepMs = expectedMs - elapsedMs;
            // No cap - if we're behind, sleepMs will be 0 or negative and we won't sleep
            
            // Sleep in 10ms intervals to allow responsive interrupts
            while (sleepMs > 0 && currentSongPlaying && !pendingReset) {
              int64_t chunk = sleepMs > 10 ? 10 : sleepMs;
              BELL_SLEEP_MS(chunk);
              sleepMs -= chunk;
              totalSleptMs += chunk;
            }
          }
          
          // Log every 30 seconds of expected playback time
          if (expectedMs - lastLogMs >= 30000) {
            CSPOT_LOG(debug, "[RATE-LIMIT] Progress: %.1fs expected, %.1fs elapsed, total slept: %.1fs", 
                     expectedMs/1000.0, elapsedMs/1000.0, totalSleptMs/1000.0);
            lastLogMs = expectedMs;
          }
        }
      }
      ov_clear(&vorbisFile);

      // Calculate final timing metrics
      auto elapsed = std::chrono::steady_clock::now() - playbackStartTime;
      auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
      auto expectedMs = (totalPCMBytes * 1000) / bytesPerSecond;

      CSPOT_LOG(info, "Playing done");
      CSPOT_LOG(info, "[RATE-LIMIT] Total sleep time: %.1fs", totalSleptMs/1000.0);
      CSPOT_LOG(info, "[ANALYSIS:TRACK_COMPLETE] expected=%lld, actual=%lld, slept=%lld",
               expectedMs, elapsedMs, totalSleptMs);

      // always move back to LOADING (ensure proper seeking after last track has been loaded)
      currentTrackStream = nullptr;
      track->loading = false;
    }

    if (eof) {
      if (trackQueue->isFinished()) {
        endOfQueueReached = true;
      }

      // Task is stopped before callbacks cleared (see CSpotPlayer destructor)
      if (this->eofCallback) {
        this->eofCallback();
      }
    }
  }
}

size_t TrackPlayer::_vorbisRead(void* ptr, size_t size, size_t nmemb) {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  return this->currentTrackStream->readBytes((uint8_t*)ptr, nmemb * size);
}

size_t TrackPlayer::_vorbisClose() {
  return 0;
}

int TrackPlayer::_vorbisSeek(int64_t offset, int whence) {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  size_t oldPos = this->currentTrackStream->getPosition();
  size_t newPos = 0;
  switch (whence) {
    case 0:
      newPos = offset;
      this->currentTrackStream->seek(offset);  // Spotify header offset
      break;
    case 1:
      newPos = this->currentTrackStream->getPosition() + offset;
      this->currentTrackStream->seek(newPos);
      break;
    case 2:
      newPos = this->currentTrackStream->getSize() + offset;
      this->currentTrackStream->seek(newPos);
      break;
  }
  
  if (newPos > oldPos + 1000000) {
    CSPOT_LOG(debug, "[VORBIS_SEEK] Large forward seek: %zu -> %zu (whence=%d, offset=%lld)",
             oldPos, newPos, whence, (long long)offset);
  }

  return 0;
}

long TrackPlayer::_vorbisTell() {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  return this->currentTrackStream->getPosition();
}

void TrackPlayer::setDataCallback(DataCallback callback) {
  this->dataCallback = callback;
}
