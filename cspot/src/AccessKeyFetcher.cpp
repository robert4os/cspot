#include "AccessKeyFetcher.h"

#include <csignal>           // for raise, SIGTERM
#include <cstdlib>           // for exit
#include <cstring>           // for strrchr
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <type_traits>       // for remove_extent_t
#include <vector>            // for vector

#include "BellLogger.h"    // for AbstractLogger
#include "BellUtils.h"      // for BELL_SLEEP_MS
#include "CSpotContext.h"  // for Context
#include "HTTPClient.h"
#include "Logger.h"            // for CSPOT_LOG
#include "MercurySession.h"    // for MercurySession, MercurySession::Res...
#include "NanoPBExtensions.h"  // for bell::nanopb::encode...
#include "NanoPBHelper.h"      // for pbEncode and pbDecode
#include "Packet.h"            // for cspot
#include "TimeProvider.h"      // for TimeProvider
#include "Utils.h"             // for string_format

#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

#include "protobuf/login5.pb.h"  // for LoginRequest

using namespace cspot;

// URL encode helper for OAuth2 POST body
static std::string urlEncode(const std::string& value) {
  std::string encoded;
  for (unsigned char c : value) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      encoded += hex;
    }
  }
  return encoded;
}

static std::string CLIENT_ID =
    "65b708073fc0480ea92a077233ca87bd";  // Spotify web client's client id

static std::string SCOPES =
    "streaming,user-library-read,user-library-modify,user-top-read,user-read-"
    "recently-played";  // Required access scopes

// Global rate limiting - shared across all AccessKeyFetcher instances
std::atomic<long long> AccessKeyFetcher::lastAttemptTimestamp{0};
std::atomic<bool> AccessKeyFetcher::globalPermanentFailure{false};

AccessKeyFetcher::AccessKeyFetcher(std::shared_ptr<cspot::Context> ctx)
    : ctx(ctx) {}

bool AccessKeyFetcher::isExpired() {
  if (accessKey.empty()) {
    return true;
  }

  if (ctx->timeProvider->getSyncedTimestamp() > expiresAt) {
    return true;
  }

  return false;
}

std::string AccessKeyFetcher::getAccessKey() {
  if (!isExpired()) {
    return accessKey;
  }

  updateAccessKey();

  return accessKey;
}

void AccessKeyFetcher::updateAccessKey() {
  // Path selection: OAuth2 vs login5
  if (!ctx->config.clientId.empty() && !ctx->config.clientSecret.empty()) {
    // Custom client with OAuth2
    CSPOT_LOG(info, "Using OAuth2 authentication (custom client)");
    return updateAccessKeyOAuth2();
  }
  
  // Default: login5 with ZeroConf credentials
  CSPOT_LOG(info, "Using login5 authentication (ZeroConf)");
  return updateAccessKeyLogin5();
}

void AccessKeyFetcher::updateAccessKeyLogin5() {
  // Check global permanent failure flag (shared across all instances)
  if (globalPermanentFailure) {
    CSPOT_LOG(error, "Skipping token fetch - global permanent failure (access token fetch failed previously)");
    return;
  }

  if (keyPending) {
    // Already pending refresh request
    return;
  }
  
  // Global rate limiting - enforce minimum interval between ANY attempts (across all instances/threads)
  long long now = ctx->timeProvider->getSyncedTimestamp();
  long long lastAttempt = lastAttemptTimestamp.load();
  if (lastAttempt > 0) {
    long long timeSinceLastAttempt = now - lastAttempt;
    if (timeSinceLastAttempt < MIN_RETRY_INTERVAL_MS) {
      long long waitTime = MIN_RETRY_INTERVAL_MS - timeSinceLastAttempt;
      CSPOT_LOG(info, "Rate limiting: waiting %lld ms before next token fetch attempt", waitTime);
      BELL_SLEEP_MS(waitTime);
    }
  }
  
  // Update timestamp (this happens before the attempt to prevent race conditions)
  lastAttemptTimestamp.store(ctx->timeProvider->getSyncedTimestamp());

  keyPending = true;

  // Prepare a protobuf login request
  static LoginRequest loginRequest = LoginRequest_init_zero;
  static LoginResponse loginResponse = LoginResponse_init_zero;

  // Use custom client ID if provided in config, otherwise use default
  std::string clientId = !ctx->config.clientId.empty() ? ctx->config.clientId : CLIENT_ID;
  
  // Log which client ID is being used
  if (!ctx->config.clientId.empty()) {
    CSPOT_LOG(info, "Using custom client ID: %s", clientId.c_str());
  } else {
    CSPOT_LOG(info, "Using default client ID: %s", clientId.c_str());
  }
  
  // Testing: ZeroConf credentials should work with any client_id
  // Validation temporarily disabled to test this hypothesis
  // if (!ctx->config.clientId.empty() && ctx->config.clientCredentials.empty()) {
  //   CSPOT_LOG(error, "Custom client ID provided without client credentials - this may cause authentication issues");
  // }
  
  // Note: clientCredentials not currently used in login5 flow (public client)
  // ZeroConf authData should be independent of client_id

  // Assign necessary request fields
  loginRequest.client_info.client_id.funcs.encode = &bell::nanopb::encodeString;
  loginRequest.client_info.client_id.arg = &clientId;

  loginRequest.client_info.device_id.funcs.encode = &bell::nanopb::encodeString;
  loginRequest.client_info.device_id.arg = &ctx->config.deviceId;

  loginRequest.login_method.stored_credential.username.funcs.encode =
      &bell::nanopb::encodeString;
  loginRequest.login_method.stored_credential.username.arg =
      &ctx->config.username;

  // Set login method to stored credential
  loginRequest.which_login_method = LoginRequest_stored_credential_tag;
  loginRequest.login_method.stored_credential.data.funcs.encode =
      &bell::nanopb::encodeVector;
  loginRequest.login_method.stored_credential.data.arg = &ctx->config.authData;

  // Retry with exponential backoff: 1s, 2s, 5s
  int retryCount = 3;
  bool success = false;
  int attemptNumber = 0;

  do {
    attemptNumber++;
    auto encodedRequest = pbEncode(LoginRequest_fields, &loginRequest);
    CSPOT_LOG(info, "Access token expired, fetching new one... %d",
              encodedRequest.size());

    // Perform a login5 request, containing the encoded protobuf data
    auto response = bell::HTTPClient::post(
        "https://login5.spotify.com/v3/login",
        {{"Content-Type", "application/x-protobuf"}}, encodedRequest);

    // Check HTTP status
    int statusCode = response->statusCode();
    if (statusCode != 200) {
      std::string retryAfter = std::string(response->header("retry-after"));
      CSPOT_LOG(error, "Access token fetch failed: HTTP %d%s", 
               statusCode,
               retryAfter.empty() ? "" : (" Retry-After: " + retryAfter).c_str());
      
      // Apply exponential backoff before retrying or exiting
      if (retryCount > 0) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        CSPOT_LOG(info, "Retrying in %d seconds... (%d attempts remaining)", delayMs/1000, retryCount);
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        CSPOT_LOG(error, "FATAL: Cannot obtain access token - HTTP error from Spotify (all retries exhausted)");
        globalPermanentFailure = true;  // Block all future attempts globally
        keyPending = false;
        
        // Trigger application shutdown via signal (allows proper cleanup)
        CSPOT_LOG(error, "Triggering application shutdown...");
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);  // Give signal time to propagate
        exit(1);  // Fallback if signal doesn't work
      }
    }

    auto responseBytes = response->bytes();

    // Deserialize the response
    pbDecode(loginResponse, LoginResponse_fields, responseBytes);

    if (loginResponse.which_response == LoginResponse_ok_tag) {
      // Successfully received an auth token
      accessKey = std::string(loginResponse.response.ok.access_token);
      
      // Get expiration from response (default 30 minutes if not provided)
      int expiresIn = 1800;  // 30 minutes default
      
      if (loginResponse.response.ok.has_access_token_expires_in) {
        expiresIn = loginResponse.response.ok.access_token_expires_in;
        CSPOT_LOG(info, "Access token fetched successfully (expires in %d seconds)", expiresIn);
      } else {
        CSPOT_LOG(info, "Access token fetched successfully (using default 30min expiration)");
      }

      this->expiresAt =
          ctx->timeProvider->getSyncedTimestamp() + (expiresIn * 1000);
      success = true;
    } else if (loginResponse.which_response == LoginResponse_error_tag) {
      // Detailed error reporting
      const char* errorName = "UNKNOWN";
      switch (loginResponse.response.error) {
        case LoginError_INVALID_CREDENTIALS: errorName = "INVALID_CREDENTIALS"; break;
        case LoginError_BAD_REQUEST: errorName = "BAD_REQUEST"; break;
        case LoginError_UNSUPPORTED_LOGIN_PROTOCOL: errorName = "UNSUPPORTED_LOGIN_PROTOCOL"; break;
        case LoginError_TIMEOUT: errorName = "TIMEOUT"; break;
        case LoginError_UNKNOWN_IDENTIFIER: errorName = "UNKNOWN_IDENTIFIER"; break;
        case LoginError_TOO_MANY_ATTEMPTS: errorName = "TOO_MANY_ATTEMPTS"; break;
        case LoginError_INVALID_PHONENUMBER: errorName = "INVALID_PHONENUMBER"; break;
        case LoginError_TRY_AGAIN_LATER: errorName = "TRY_AGAIN_LATER"; break;
        default: break;
      }
      CSPOT_LOG(error, "Access token fetch failed: Spotify returned error: %s (%d)", 
               errorName, loginResponse.response.error);
      
      // Check if this is a permanent error that should not be retried
      bool isPermanentError = (loginResponse.response.error == LoginError_INVALID_CREDENTIALS ||
                               loginResponse.response.error == LoginError_BAD_REQUEST ||
                               loginResponse.response.error == LoginError_UNSUPPORTED_LOGIN_PROTOCOL);
      
      // Apply exponential backoff before retrying or exiting (only for transient errors)
      if (retryCount > 0 && !isPermanentError) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        CSPOT_LOG(info, "Retrying in %d seconds... (%d attempts remaining)", delayMs/1000, retryCount);
        pb_release(LoginResponse_fields, &loginResponse);
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        if (isPermanentError) {
          CSPOT_LOG(error, "FATAL: Permanent authentication error - cannot be fixed by retrying");
        } else {
          CSPOT_LOG(error, "FATAL: Cannot obtain access token - all retries exhausted");
        }
        
        if (loginResponse.response.error == LoginError_BAD_REQUEST && !ctx->config.clientId.empty()) {
          CSPOT_LOG(error, "       ZeroConf credentials are bound to original client_id");
          CSPOT_LOG(error, "       Either remove custom client_id or re-pair device with new client_id");
        } else if (loginResponse.response.error == LoginError_INVALID_CREDENTIALS) {
          CSPOT_LOG(error, "       Credentials are invalid or expired - re-authenticate required");
        }
        
        globalPermanentFailure = true;  // Block all future attempts globally
        pb_release(LoginResponse_fields, &loginResponse);
        keyPending = false;
        
        // Trigger application shutdown via signal (allows proper cleanup)
        CSPOT_LOG(error, "Triggering application shutdown...");
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);  // Give signal time to propagate
        exit(1);  // Fallback if signal doesn't work
      }
    } else {
      CSPOT_LOG(error, "Access token fetch failed: Unknown response type %d", loginResponse.which_response);
      
      // Apply exponential backoff before retrying or exiting
      if (retryCount > 0) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        CSPOT_LOG(info, "Retrying in %d seconds... (%d attempts remaining)", delayMs/1000, retryCount);
        pb_release(LoginResponse_fields, &loginResponse);
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        CSPOT_LOG(error, "FATAL: Cannot obtain access token - unknown protocol error (all retries exhausted)");
        globalPermanentFailure = true;  // Block all future attempts globally
        pb_release(LoginResponse_fields, &loginResponse);
        keyPending = false;
        
        // Trigger application shutdown via signal (allows proper cleanup)
        CSPOT_LOG(error, "Triggering application shutdown...");
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);  // Give signal time to propagate
        exit(1);  // Fallback if signal doesn't work
      }
    }

    // Free up allocated memory for response
    pb_release(LoginResponse_fields, &loginResponse);

    retryCount--;
  } while (retryCount >= 0 && !success);

  keyPending = false;
}

void AccessKeyFetcher::updateAccessKeyOAuth2() {
  // Check global permanent failure flag (shared across all instances)
  if (globalPermanentFailure) {
    CSPOT_LOG(error, "Skipping OAuth2 token refresh - global permanent failure");
    return;
  }

  if (keyPending) {
    // Already pending refresh request
    return;
  }

  // Global rate limiting - enforce minimum interval between ANY attempts
  long long now = ctx->timeProvider->getSyncedTimestamp();
  long long lastAttempt = lastAttemptTimestamp.load();
  if (lastAttempt > 0) {
    long long timeSinceLastAttempt = now - lastAttempt;
    if (timeSinceLastAttempt < MIN_RETRY_INTERVAL_MS) {
      long long waitTime = MIN_RETRY_INTERVAL_MS - timeSinceLastAttempt;
      CSPOT_LOG(info, "Rate limiting: waiting %lld ms before next OAuth2 token refresh", waitTime);
      BELL_SLEEP_MS(waitTime);
    }
  }

  // Update timestamp (before attempt to prevent race conditions)
  lastAttemptTimestamp.store(ctx->timeProvider->getSyncedTimestamp());

  keyPending = true;

  // Parse OAuth tokens from context
  if (ctx->config.oauthTokens.empty()) {
    CSPOT_LOG(error, "FATAL: OAuth2 mode but no tokens provided");
    CSPOT_LOG(error, "       Token file should exist: spotupnp-client-{clientid}.json");
    globalPermanentFailure = true;
    keyPending = false;
    std::raise(SIGTERM);
    BELL_SLEEP_MS(1000);
    exit(1);
  }

#ifdef BELL_ONLY_CJSON
  cJSON* tokens = cJSON_Parse(ctx->config.oauthTokens.c_str());
  if (!tokens) {
    CSPOT_LOG(error, "FATAL: Failed to parse OAuth tokens JSON");
    globalPermanentFailure = true;
    keyPending = false;
    std::raise(SIGTERM);
    BELL_SLEEP_MS(1000);
    exit(1);
  }

  cJSON* accessTokenItem = cJSON_GetObjectItem(tokens, "access_token");
  cJSON* refreshTokenItem = cJSON_GetObjectItem(tokens, "refresh_token");
  cJSON* expiresAtItem = cJSON_GetObjectItem(tokens, "expires_at");

  if (!refreshTokenItem || !cJSON_IsString(refreshTokenItem)) {
    CSPOT_LOG(error, "FATAL: No refresh_token in OAuth tokens");
    cJSON_Delete(tokens);
    globalPermanentFailure = true;
    keyPending = false;
    std::raise(SIGTERM);
    BELL_SLEEP_MS(1000);
    exit(1);
  }

  std::string currentAccessToken = accessTokenItem && cJSON_IsString(accessTokenItem) ? 
                                   accessTokenItem->valuestring : "";
  std::string refreshToken = refreshTokenItem->valuestring;
  long long expiresAtSeconds = expiresAtItem && cJSON_IsNumber(expiresAtItem) ? 
                               expiresAtItem->valueint : 0;
  
  cJSON_Delete(tokens);
#else
  nlohmann::json tokens;
  try {
    tokens = nlohmann::json::parse(ctx->config.oauthTokens);
  } catch (const std::exception& e) {
    CSPOT_LOG(error, "FATAL: Failed to parse OAuth tokens JSON: %s", e.what());
    globalPermanentFailure = true;
    keyPending = false;
    std::raise(SIGTERM);
    BELL_SLEEP_MS(1000);
    exit(1);
  }

  if (!tokens.contains("refresh_token")) {
    CSPOT_LOG(error, "FATAL: No refresh_token in OAuth tokens");
    globalPermanentFailure = true;
    keyPending = false;
    std::raise(SIGTERM);
    BELL_SLEEP_MS(1000);
    exit(1);
  }

  std::string currentAccessToken = tokens.value("access_token", "");
  std::string refreshToken = tokens["refresh_token"];
  long long expiresAtSeconds = tokens.value("expires_at", 0LL);
#endif

  // Check if current access token is still valid (with 60 second margin)
  long long currentTimestamp = ctx->timeProvider->getSyncedTimestamp() / 1000;  // Convert to seconds
  if (!currentAccessToken.empty() && expiresAtSeconds > (currentTimestamp + 60)) {
    CSPOT_LOG(info, "OAuth2 access token still valid (expires in %lld seconds)", 
              expiresAtSeconds - currentTimestamp);
    accessKey = currentAccessToken;
    expiresAt = expiresAtSeconds * 1000;  // Convert back to milliseconds
    keyPending = false;
    return;
  }

  CSPOT_LOG(info, "OAuth2 access token expired, refreshing...");

  // Build URL-encoded POST body
  std::string postBody = "grant_type=refresh_token";
  postBody += "&refresh_token=" + urlEncode(refreshToken);
  postBody += "&client_id=" + urlEncode(ctx->config.clientId);
  postBody += "&client_secret=" + urlEncode(ctx->config.clientSecret);

  // Retry with exponential backoff: 1s, 2s, 5s
  int retryCount = 3;
  bool success = false;
  int attemptNumber = 0;

  do {
    attemptNumber++;
    
    // Convert postBody string to vector<uint8_t>
    std::vector<uint8_t> postBodyBytes(postBody.begin(), postBody.end());
    
    // POST to Spotify OAuth token endpoint
    auto response = bell::HTTPClient::post(
        "https://accounts.spotify.com/api/token",
        {{"Content-Type", "application/x-www-form-urlencoded"}},
        postBodyBytes);

    // Check HTTP status
    int statusCode = response->statusCode();
    if (statusCode != 200) {
      CSPOT_LOG(error, "OAuth2 token refresh failed: HTTP %d", statusCode);
      
      // Apply exponential backoff before retrying or exiting
      if (retryCount > 0) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        CSPOT_LOG(info, "Retrying in %d seconds... (%d attempts remaining)", delayMs/1000, retryCount);
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        CSPOT_LOG(error, "FATAL: Cannot refresh OAuth2 token - HTTP error (all retries exhausted)");
        globalPermanentFailure = true;
        keyPending = false;
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);
        exit(1);
      }
    }

    // Parse JSON response
    std::string_view responseBody = response->body();

#ifdef BELL_ONLY_CJSON
    cJSON* json = cJSON_Parse(responseBody.data());
    if (!json) {
      CSPOT_LOG(error, "OAuth2 token refresh failed: Invalid JSON response");
      if (retryCount > 0) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        globalPermanentFailure = true;
        keyPending = false;
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);
        exit(1);
      }
    }

    cJSON* errorItem = cJSON_GetObjectItem(json, "error");
    if (errorItem && cJSON_IsString(errorItem)) {
      std::string error = errorItem->valuestring;
      CSPOT_LOG(error, "OAuth2 token refresh failed: %s", error.c_str());
      
      // Check for permanent errors
      bool isPermanentError = (error == "invalid_grant" || error == "invalid_client");
      
      if (isPermanentError) {
        if (error == "invalid_grant") {
          CSPOT_LOG(error, "FATAL: Refresh token expired or revoked - user must re-authenticate");
        } else {
          CSPOT_LOG(error, "FATAL: Invalid client_id or client_secret");
        }
        cJSON_Delete(json);
        globalPermanentFailure = true;
        keyPending = false;
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);
        exit(1);
      }
      
      cJSON_Delete(json);
      if (retryCount > 0) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        globalPermanentFailure = true;
        keyPending = false;
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);
        exit(1);
      }
    }

    cJSON* newAccessTokenItem = cJSON_GetObjectItem(json, "access_token");
    cJSON* expiresInItem = cJSON_GetObjectItem(json, "expires_in");
    
    if (!newAccessTokenItem || !cJSON_IsString(newAccessTokenItem)) {
      CSPOT_LOG(error, "OAuth2 token refresh failed: No access_token in response");
      cJSON_Delete(json);
      if (retryCount > 0) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        globalPermanentFailure = true;
        keyPending = false;
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);
        exit(1);
      }
    }

    accessKey = newAccessTokenItem->valuestring;
    int expiresIn = expiresInItem && cJSON_IsNumber(expiresInItem) ? expiresInItem->valueint : 3600;
    
    cJSON_Delete(json);
#else
    nlohmann::json json;
    try {
      json = nlohmann::json::parse(responseBody);
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "OAuth2 token refresh failed: Invalid JSON response: %s", e.what());
      if (retryCount > 0) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        globalPermanentFailure = true;
        keyPending = false;
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);
        exit(1);
      }
    }

    if (json.contains("error")) {
      std::string error = json["error"];
      CSPOT_LOG(error, "OAuth2 token refresh failed: %s", error.c_str());
      
      // Check for permanent errors
      bool isPermanentError = (error == "invalid_grant" || error == "invalid_client");
      
      if (isPermanentError) {
        if (error == "invalid_grant") {
          CSPOT_LOG(error, "FATAL: Refresh token expired or revoked - user must re-authenticate");
        } else {
          CSPOT_LOG(error, "FATAL: Invalid client_id or client_secret");
        }
        globalPermanentFailure = true;
        keyPending = false;
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);
        exit(1);
      }
      
      if (retryCount > 0) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        globalPermanentFailure = true;
        keyPending = false;
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);
        exit(1);
      }
    }

    if (!json.contains("access_token")) {
      CSPOT_LOG(error, "OAuth2 token refresh failed: No access_token in response");
      if (retryCount > 0) {
        int delayMs = (attemptNumber == 1) ? 1000 : (attemptNumber == 2) ? 2000 : 5000;
        BELL_SLEEP_MS(delayMs);
        retryCount--;
        continue;
      } else {
        globalPermanentFailure = true;
        keyPending = false;
        std::raise(SIGTERM);
        BELL_SLEEP_MS(1000);
        exit(1);
      }
    }

    accessKey = json["access_token"];
    int expiresIn = json.value("expires_in", 3600);
#endif

    // Calculate new expiration timestamp
    long long newExpiresAt = (ctx->timeProvider->getSyncedTimestamp() / 1000) + expiresIn;
    expiresAt = newExpiresAt * 1000;  // Store in milliseconds
    
    CSPOT_LOG(info, "OAuth2 token refreshed successfully (expires in %d seconds)", expiresIn);

    // Save updated tokens back to file
    // Build new token JSON with updated access_token and expires_at
#ifdef BELL_ONLY_CJSON
    cJSON* updatedTokens = cJSON_CreateObject();
    cJSON_AddStringToObject(updatedTokens, "access_token", accessKey.c_str());
    cJSON_AddStringToObject(updatedTokens, "refresh_token", refreshToken.c_str());
    cJSON_AddNumberToObject(updatedTokens, "expires_at", newExpiresAt);
    cJSON_AddStringToObject(updatedTokens, "token_type", "Bearer");
    
    char* tokensStr = cJSON_PrintUnformatted(updatedTokens);
    std::string updatedTokensJson(tokensStr);
    free(tokensStr);
    cJSON_Delete(updatedTokens);
#else
    nlohmann::json updatedTokens;
    updatedTokens["access_token"] = accessKey;
    updatedTokens["refresh_token"] = refreshToken;
    updatedTokens["expires_at"] = newExpiresAt;
    updatedTokens["token_type"] = "Bearer";
    std::string updatedTokensJson = updatedTokens.dump();
#endif

    // Update context (for next time)
    ctx->config.oauthTokens = updatedTokensJson;
    
    // Trigger save to file via callback
    if (ctx->config.oauthTokenSaveCallback) {
      ctx->config.oauthTokenSaveCallback(ctx->config.clientId, updatedTokensJson);
      CSPOT_LOG(info, "OAuth2 tokens saved to file");
    } else {
      CSPOT_LOG(error, "OAuth2 tokens updated in memory only (no save callback)");
    }
    
    success = true;
    retryCount--;
  } while (retryCount >= 0 && !success);

  keyPending = false;
}
