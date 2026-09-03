/* The engine: one call at a time, and everything needed to start or answer
 * one.
 *
 * This is avenginekit.js's WfcAVEngineKit. It owns the routing -- picking the
 * VOIP messages out of the IM stream and turning them into call state -- and
 * it owns the single current session, because a board with one speaker and
 * one microphone can be in exactly one call.
 *
 * ------------------------------------------------------------------------
 * How it hangs off the IM client.
 *
 * There is no second connection and no second protocol. WFC carries call
 * control as ordinary IM messages (types 400..417, wfc_model.h) and carries
 * the Janus room protocol on the CONF topic of the same long link. So the
 * engine subscribes to two of the client's events and sends on two of its
 * calls:
 *
 *   in    wfc_on_receive_messages()   invite, accept, bye
 *   in    wfc_on_conference_event()   Janus pushed something (CONFN)
 *   out   wfc_send_message()          invite, accept, bye
 *   out   wfc_send_conference_request()  create_room, join, publish, leave
 *
 * That is the whole seam. If the long link is down there is no call to be
 * had, and the engine reports it rather than queueing.
 *
 * ------------------------------------------------------------------------
 * What this build does and does not do.
 *
 * Does: answer an incoming 1v1 audio call, start one, hang up, mute, and
 * report who is in the room. Opus at 16 kHz mono, AEC on the capture side,
 * media forced through the TURN relay.
 *
 * Does not: video in either direction (ASSESSMENT.md 10.6 -- the board has no
 * camera and cannot decode a useful frame rate), conferences with a host and
 * an audience, screen sharing, remote control, or inviting a third person
 * into a call in progress. Group calls are refused rather than half-answered:
 * a group invite is replied to with a busy bye, so the group is not left
 * waiting for a participant that will never publish.
 */

#ifndef WFAV_ENGINE_H
#define WFAV_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfav_event.h"
#include "wfav_session.h"
#include "wfav_types.h"
#include "wfc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One TURN/STUN server. WFC does not hand these out at connect time the way
 * some deployments do -- they are client configuration, read from
 * Config.ICE_SERVERS in the JS reference and from Kconfig here -- so a
 * deployment with its own TURN sets them in sdkconfig and nothing else
 * changes. */
typedef struct {
    const char *url;        /* "turn:turn.example.com:3478" or "stun:..." */
    const char *username;
    const char *password;
} wfav_ice_server_t;

#define WFAV_MAX_ICE_SERVERS 2

typedef struct {
    wfav_ice_server_t ice_servers[WFAV_MAX_ICE_SERVERS];
    size_t            n_ice_servers;

    /* Only pair relay candidates -- FORCE_RELAY in the JS reference. Off, as
     * it is there.
     *
     * It was on by default here for one release, on the theory that skipping
     * host and server-reflexive candidates left a connection path with one
     * way to fail instead of a dozen (ASSESSMENT.md risk R8). That argument
     * is backwards when Janus is reachable directly: forcing relay does not
     * remove failure modes, it removes the paths that work and makes the call
     * depend entirely on the TURN server. The first call ever placed from
     * this board hung in CONNECTING with the room reporting `joining`
     * followed by `unpublished` and no `publishers` in between -- the offer
     * reached Janus, and no candidate pair ever came up under it.
     *
     * Turn it back on only for a deployment whose Janus is genuinely
     * unreachable except through TURN. */
    bool force_relay;

    /* Seconds of ringing before an unanswered call gives up, at both ends.
     * 0 -> 60, which is what every other WFC client uses; changing it on one
     * end only means one end hangs up first and the other reports it as a
     * remote hangup rather than a timeout. */
    int ring_timeout_s;
} wfav_engine_config_t;

/* Fills `cfg` with the built-in defaults: the ICE servers from Kconfig, relay
 * NOT forced (see force_relay above), 60 second ring. Call it, change what
 * you need, pass it on. */
void wfav_engine_default_config(wfav_engine_config_t *cfg);

/* Starts the AV task and subscribes to the IM client's events.
 *
 * Call after wfc_client_init() and before wfc_client_connect(), so that an
 * invite arriving in the first seconds of the link is not missed. It does not
 * touch the audio hardware: the codec is opened when a call starts and closed
 * when it ends, because holding the I2S channels open costs DMA buffers in
 * internal SRAM for as long as the board is on. */
esp_err_t wfav_engine_start(const wfav_engine_config_t *cfg);

/* Hangs up anything in progress and stops the task. Never call from an event
 * callback -- it waits for the task the callback is running on. */
void wfav_engine_stop(void);

/* ------------------------------------------------------------- starting */

/* Calls `user_id`, audio only.
 *
 * Returns immediately: the room is created and the invite sent on the AV
 * task, and the call appears through wfav_on_call_started() a moment later
 * whether or not it succeeds. ESP_ERR_INVALID_STATE means a call is already
 * up; ESP_ERR_NOT_SUPPORTED means the long link is down.
 *
 * Safe from any task, including the UI's -- which matters, because the button
 * that calls this runs on the LVGL task with the display lock held. */
esp_err_t wfav_start_call(const char *user_id);

/* True while there is a call in any state but IDLE. The cheap test a
 * conversation screen uses to decide whether its call button is live. */
bool wfav_is_busy(void);

/* ------------------------------------------------------------- speaker */

/* The speaker, 0..100. Not part of the call protocol and not part of any one
 * call -- it is the codec's output volume, and a board turned down stays
 * turned down -- but it belongs on the call screen, so it is here rather than
 * making a UI reach for esp_codec_dev. */
esp_err_t wfav_set_speaker_volume(int percent);
int       wfav_get_speaker_volume(void);

#ifdef __cplusplus
}
#endif

#endif /* WFAV_ENGINE_H */
