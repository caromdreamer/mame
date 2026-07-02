/*
  Kaileron rollback client API draft.

  This is the emulator-facing C ABI boundary. The rollback client owns
  networking, prediction, confirmed input handling, rollback, and resimulation.
  The emulator owns input encoding, save/load implementation, and deterministic
  frame execution.
*/

#ifndef KAILERON_H
#define KAILERON_H

#include <stdint.h>

#if defined(_WIN32)
#define KN_CALL __stdcall
#if defined(KAILERON_DLL)
#define KN_API __declspec(dllexport)
#else
#define KN_API __declspec(dllimport)
#endif
#else
#define KN_CALL
#define KN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KN_API_VERSION 7
#define KN_MAX_PLAYERS 4

typedef struct KnClient KnClient;
typedef struct KnHostSession KnHostSession;
typedef struct KnLobbyClient KnLobbyClient;

typedef enum KnResult {
  KN_OK = 0,
  KN_ERR_INVALID_ARGUMENT = -1,
  KN_ERR_NETWORK = -2,
  KN_ERR_CALLBACK = -3,
  KN_ERR_NOT_READY = -4,
  KN_ERR_INTERNAL = -5,
  KN_ERR_REPLAY_COMPLETE = -6
} KnResult;

typedef struct KnInput {
  const uint8_t *bytes;
  uint32_t len;
} KnInput;

typedef struct KnMutableInput {
  uint8_t *bytes;
  uint32_t cap;
  uint32_t len;
} KnMutableInput;

typedef struct KnNetProfile {
  /*
    Test transport impairment for outbound SDK packets. This is for validation
    hosts and should normally be zeroed in production integrations.
  */
  uint32_t delay_ms;
  uint32_t jitter_ms;
  uint32_t loss_percent;
} KnNetProfile;

typedef enum KnPlaybackPhase {
  KN_PLAYBACK_LIVE = 0,
  KN_PLAYBACK_CATCHING_UP = 1,
  KN_PLAYBACK_STALLED = 2
} KnPlaybackPhase;

typedef struct KnPlaybackControl {
  /*
    Descriptive state for UI/logging. Hosts should not infer playback policy
    from phase; apply the command fields below.
  */
  uint32_t phase;
  uint32_t current_frame;
  uint32_t latest_frame;
  uint32_t backlog_frames;
  /*
    Commanded emulator speed. 100 means normal speed; values above 100 request
    bounded catch-up playback. Hosts may clamp to their supported range.
  */
  uint32_t target_speed_percent;
  uint32_t render_interval;
  uint32_t mute_audio;
  /*
    SDK-owned host pacing hint for the caller's main loop. Zero means the host
    should not add a sleep for this tick.
  */
  uint32_t pace_delay_us;
  uint32_t reserved[8];
} KnPlaybackControl;

typedef struct KnPlaybackStatus {
  /*
    UI-facing snapshot owned by the SDK. Hosts can display the strings as-is or
    use the numeric fields for localization/custom UI.
  */
  uint32_t phase;
  uint32_t current_frame;
  uint32_t latest_frame;
  uint32_t backlog_frames;
  uint32_t target_speed_percent;
  uint32_t render_interval;
  uint32_t mute_audio;
  uint32_t pace_delay_us;
  uint32_t reserved[8];
  char short_text[32];
  char detail_text[128];
} KnPlaybackStatus;

typedef enum KnLifecycleEventType {
  KN_LIFECYCLE_SESSION_CREATED = 1,
  KN_LIFECYCLE_SESSION_CONNECTING = 2,
  KN_LIFECYCLE_SESSION_WELCOMED = 3,
  KN_LIFECYCLE_SESSION_READY_SENT = 4,
  KN_LIFECYCLE_SESSION_STARTING = 5,
  KN_LIFECYCLE_SESSION_STARTED = 6,
  KN_LIFECYCLE_SESSION_LEFT = 7,
  KN_LIFECYCLE_PEER_LEFT = 20,
  KN_LIFECYCLE_ROLLBACK_BEGIN = 40,
  KN_LIFECYCLE_ROLLBACK_END = 41,
  KN_LIFECYCLE_ERROR = 60
} KnLifecycleEventType;

typedef enum KnLifecycleErrorReason {
  KN_LIFECYCLE_ERROR_SERVER_TIMEOUT = 1
} KnLifecycleErrorReason;

typedef struct KnLifecycleEvent {
  uint32_t type;
  uint32_t frame;
  uint32_t peer_id;
  uint32_t reason;
  uint32_t reserved[8];
} KnLifecycleEvent;

typedef struct KnCallbacks {
  /*
    Set to sizeof(KnCallbacks). This gives future SDKs room to accept older
    hosts or discover newly added optional callbacks explicitly.
  */
  uint32_t struct_size;
  void *user;

  /*
    Fill out_input with the local player's opaque input bytes for input_frame.
    The SDK does not interpret the bytes.
  */
  KnResult(KN_CALL *poll_local_input)(void *user,
                                      uint32_t input_frame,
                                      KnMutableInput *out_input);

  /*
    Save and restore emulator state addressed by frame number.
    Snapshot bytes or slots remain owned by the emulator integration.
  */
  KnResult(KN_CALL *save_state)(void *user, uint32_t frame);
  KnResult(KN_CALL *load_state)(void *user, uint32_t frame);
  void(KN_CALL *discard_states_before)(void *user, uint32_t frame);

  /*
    Advance exactly one deterministic emulated frame using all players' input
    byte arrays for that frame. players has player_count entries.
  */
  KnResult(KN_CALL *advance_frame)(void *user,
                                   uint32_t frame,
                                   const KnInput *players,
                                   uint32_t player_count);

  /*
    Optional but strongly recommended for test builds. Return a deterministic
    hash of the current emulator state after serialization or equivalent.
  */
  uint64_t(KN_CALL *state_hash)(void *user);

  /*
    Optional playback policy hook. The SDK owns catch-up policy and calls this
    when it wants the host to adjust speed, rendering cadence, or audio while
    replaying confirmed history.
  */
  void(KN_CALL *set_playback_control)(void *user,
                                      const KnPlaybackControl *control);
  /*
    Optional lifecycle notification stream for session, peer, and rollback
    state changes. Hosts may ignore it and continue polling metrics/status.
  */
  void(KN_CALL *on_lifecycle_event)(void *user,
                                    const KnLifecycleEvent *event);
  uint32_t reserved[7];
} KnCallbacks;

typedef struct KnConfig {
  /*
    Set to sizeof(KnConfig). Keep this first so future SDKs can validate the
    host's view of the ABI before reading newly added fields.
  */
  uint32_t struct_size;
  uint32_t api_version;
  const char *server_addr;
  const char *session_name;
  uint32_t player_id;
  uint32_t spectator;
  uint64_t spectator_id;
  const char *lobby_url;
  uint32_t player_count;
  /*
    Fixed opaque bytes per player's input. Pass 0 to let the SDK infer the
    size from the first poll_local_input callback before joining a session.
  */
  uint32_t input_size;
  uint32_t input_delay_frames;
  uint32_t max_rollback_frames;
  uint32_t max_prediction_frames;
  /*
    Emulated frame duration in microseconds. Pass 0 to use the SDK default
    of 60Hz. The SDK uses this for spectator catch-up policy and UI text.
  */
  uint32_t frame_duration_us;
  uint32_t host_capabilities;
  uint32_t reserved[8];
  KnNetProfile net_profile;
} KnConfig;

typedef struct KnMetrics {
  uint32_t current_frame;
  uint32_t confirmed_frame_count;
  uint32_t rollback_count;
  uint32_t max_rollback_frames;
  uint32_t sent_packets;
  uint32_t dropped_packets;
  uint32_t defaulted_inputs;
  uint32_t own_defaulted_inputs;
  uint32_t input_delay_frames;
  uint32_t max_prediction_frames;
  uint32_t prediction_stall_frames;
  uint32_t disconnected_mask;
  uint32_t neutral_provider_frames;
  uint32_t confirmed_nack_sent;
  uint32_t confirmed_nack_frames_requested;
  uint32_t confirmed_missing_frames;
  uint32_t spectator_buffered_frames;
  uint32_t spectator_latest_frame;
  uint64_t confirmed_input_hash;
  uint64_t current_state_hash;
} KnMetrics;

typedef struct KnLobbyCreateRoom {
  /*
    Optional. Pass NULL to let the lobby server generate a room id.
    Explicit room ids may contain ASCII letters, digits, '-' and '_'.
  */
  const char *room_id;
  uint32_t player_count;
  uint32_t input_size;
  const char *core_id;
  const char *game_hash;
  uint32_t protocol_version;
} KnLobbyCreateRoom;

typedef struct KnLobbyRoom {
  char *room_id;
  uint32_t player_count;
  uint32_t joined_players;
  uint32_t ready_players;
  uint32_t started;
  uint32_t realtime_started;
  uint32_t input_size;
  char *core_id;
  char *game_hash;
  uint32_t protocol_version;
} KnLobbyRoom;

typedef struct KnLobbyJoinResult {
  char *session;
  uint32_t player_id;
  char *udp_addr;
  uint32_t player_count;
  uint32_t input_size;
  char *core_id;
  char *game_hash;
  uint32_t protocol_version;
} KnLobbyJoinResult;

typedef struct KnLobbyStartResult {
  char *session;
  uint32_t started;
  char *udp_addr;
} KnLobbyStartResult;

KN_API const char *KN_CALL kn_get_version(void);

/*
  Initialize SDK-owned structs with ABI-safe defaults.

  Hosts may still stack-allocate these structs, but should prefer these helpers
  over hand-written memset/struct_size/api_version boilerplate. The functions
  live in the SDK shared library, so they are usable from any FFI-capable host
  language without shipping extra helper source files.
*/
KN_API KnResult KN_CALL kn_config_init(KnConfig *config);
KN_API KnResult KN_CALL kn_callbacks_init(KnCallbacks *callbacks, void *user);
KN_API KnResult KN_CALL kn_playback_status_init(KnPlaybackStatus *status);

KN_API KnResult KN_CALL kn_client_create(const KnConfig *config,
                                         const KnCallbacks *callbacks,
                                         KnClient **out_client);

KN_API void KN_CALL kn_client_destroy(KnClient *client);

/*
  Drive one rollback-client scheduler tick.

  The SDK may call poll_local_input, save_state, load_state, advance_frame, and
  discard_states_before from inside this function. Host emulators should call
  this from the deterministic emulation thread or otherwise serialize access to
  emulator state.
*/
KN_API KnResult KN_CALL kn_client_tick(KnClient *client);

/*
  Receive and apply pending network messages without advancing a new emulated
  frame. Useful when a host has reached its local frame budget and only wants
  to settle confirmations.
*/
KN_API KnResult KN_CALL kn_client_poll_network(KnClient *client);

/*
  Apply one server-confirmed input frame.

  The normal DLL build will call this from its network receive path. It is
  exposed in the draft ABI so tests and alternate transports can validate the
  rollback boundary before the transport is finalized.
*/
KN_API KnResult KN_CALL kn_client_apply_confirmed_frame(KnClient *client,
                                                        uint32_t frame,
                                                        const KnInput *players,
                                                        uint32_t player_count);

KN_API KnResult KN_CALL kn_client_get_metrics(KnClient *client,
                                              KnMetrics *out_metrics);

KN_API KnResult KN_CALL kn_client_get_playback_control(
    KnClient *client,
    KnPlaybackControl *out_control);

KN_API KnResult KN_CALL kn_client_get_playback_status(
    KnClient *client,
    KnPlaybackStatus *out_status);

/*
  Notify the game-session transport that this emulator instance is leaving.

  This is intentionally separate from lobby-room membership. A host should call
  it during normal emulator shutdown so peers do not need to wait for the
  disconnect timeout before treating this player as a neutral input provider.
*/
KN_API void KN_CALL kn_client_leave(KnClient *client);

/*
  Higher-level host session wrapper.

  This wraps KnClient lifecycle without changing the callback contract. It
  forwards to the normal client API, but keeps normal shutdown smooth by sending
  leave during destroy if the host has not already called leave.
*/
KN_API KnResult KN_CALL kn_host_session_create(const KnConfig *config,
                                               const KnCallbacks *callbacks,
                                               KnHostSession **out_session);

KN_API void KN_CALL kn_host_session_destroy(KnHostSession *session);

KN_API KnResult KN_CALL kn_host_session_tick(KnHostSession *session);

KN_API KnResult KN_CALL kn_host_session_poll_network(KnHostSession *session);

KN_API KnResult KN_CALL kn_host_session_apply_confirmed_frame(
    KnHostSession *session,
    uint32_t frame,
    const KnInput *players,
    uint32_t player_count);

KN_API KnResult KN_CALL kn_host_session_get_metrics(KnHostSession *session,
                                                    KnMetrics *out_metrics);

KN_API KnResult KN_CALL kn_host_session_get_playback_control(
    KnHostSession *session,
    KnPlaybackControl *out_control);

KN_API KnResult KN_CALL kn_host_session_get_playback_status(
    KnHostSession *session,
    KnPlaybackStatus *out_status);

KN_API void KN_CALL kn_host_session_leave(KnHostSession *session);

/*
  Lobby control-plane client.

  base_url should point at the versioned lobby root, for example
  "http://127.0.0.1:40001/v1". The SDK currently accepts plain HTTP only so
  emulator-facing builds do not need TLS crypto dependencies.
*/
KN_API KnResult KN_CALL kn_lobby_client_create(const char *base_url,
                                               KnLobbyClient **out_client);

KN_API void KN_CALL kn_lobby_client_destroy(KnLobbyClient *client);

KN_API KnResult KN_CALL kn_lobby_create_room(KnLobbyClient *client,
                                             const KnLobbyCreateRoom *request,
                                             KnLobbyRoom *out_room);

KN_API void KN_CALL kn_lobby_room_destroy(KnLobbyRoom *room);

/*
  Pass player_id = -1 to request the first open player slot.
*/
KN_API KnResult KN_CALL kn_lobby_join_room(KnLobbyClient *client,
                                           const char *room_id,
                                           int32_t player_id,
                                           KnLobbyJoinResult *out_result);

KN_API void KN_CALL kn_lobby_join_result_destroy(KnLobbyJoinResult *result);

KN_API KnResult KN_CALL kn_lobby_ready(KnLobbyClient *client,
                                       const char *room_id,
                                       uint32_t player_id,
                                       uint32_t ready);

KN_API KnResult KN_CALL kn_lobby_start(KnLobbyClient *client,
                                       const char *room_id,
                                       uint32_t player_id,
                                       KnLobbyStartResult *out_result);

KN_API void KN_CALL kn_lobby_start_result_destroy(KnLobbyStartResult *result);

#ifdef __cplusplus
}
#endif

#endif
