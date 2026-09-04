/* The push-to-talk SDK, in one include.
 *
 * wfptt is to wfc what ptt.js is to WFC.js: a separate component that speaks
 * the same protocol over the same connection and knows nothing the IM client
 * does not already expose. Including this pulls the four headers that matter
 * -- the client that owns the channel, the audio the application lends it,
 * the events a screen subscribes to, and the vocabulary all three share.
 */

#ifndef WFPTT_H
#define WFPTT_H

#include "wfptt_audio.h"
#include "wfptt_client.h"
#include "wfptt_event.h"
#include "wfptt_types.h"

#endif /* WFPTT_H */
