/* The AV SDK, in one include.
 *
 * wfav is to wfc what avenginekit.js is to WFC.js: a separate component that
 * speaks the same protocol over the same connection and knows nothing the IM
 * client does not already expose. Including this pulls the four headers that
 * matter -- the engine that owns the call, the session a screen reads, the
 * events it subscribes to, and the vocabulary all three share.
 */

#ifndef WFAV_H
#define WFAV_H

#include "wfav_engine.h"
#include "wfav_event.h"
#include "wfav_session.h"
#include "wfav_types.h"

#endif /* WFAV_H */
