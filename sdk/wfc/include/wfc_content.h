/* Content types: the table that says what a message type means.
 *
 * This is registerMessageContent() from WFC.js, and it exists for the reason
 * the JS one does. A deployment invents message types -- a work order, a
 * sensor reading, a door-open notice -- and the client has to know three
 * things about each of them without the SDK having been rebuilt: what to put
 * on the wire's persist_flag, whether it is a notice or a bubble, and what
 * one line of it looks like in a conversation list.
 *
 * Before this table those three answers were switch statements inside
 * wfc_model.c, so adding a type meant editing the component -- which is the
 * one thing every WFC client's documentation tells an integrator not to do,
 * because it is what makes the next SDK update painful. Now the component
 * holds the built-in types in exactly the same structure an application
 * registers its own with, and an application's types are indistinguishable
 * from ours everywhere it matters: the digest the store writes on a
 * conversation row, the "is this a notice" test a chat page draws with, the
 * persist flag a send goes out with.
 *
 * ------------------------------------------------------------------------
 * Registering. Call wfc_register_content_type() (or the plural form) once,
 * at startup, after wfc_client_init() and before wfc_client_connect() --
 * before there is any traffic, and from one task. The table is read from the
 * wfc task (a message arriving is filed with its digest) and from the UI task
 * (a page drawing that digest); registering while either is running is a
 * race, and there is no lock here because startup registration does not need
 * one.
 *
 * A registration for a type that is already known REPLACES it, built-in types
 * included -- which is how an application puts its own words on the screen
 * ("[图片]" rather than "image") without a fork. The component ships no
 * localized strings for this reason: the built-in names are ASCII labels
 * meant for logs, and the application decides what a person reads.
 *
 * `name`, `digest` and the struct's other pointers are NOT copied. Register
 * static storage -- a file-scope const array is the intended shape, the same
 * one CustomMessageConfig.CustomMessageContents has in the web client.
 *
 * By WFC's convention types below 1000 are the protocol's and types from 1000
 * up are a deployment's. Nothing here enforces it; it is what keeps a custom
 * type from colliding with one WFC adds later.
 */

#ifndef WFC_CONTENT_H
#define WFC_CONTENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where a deployment's own types start. */
#define WFC_CONTENT_CUSTOM_FIRST 1000

/* wfc_content_out_t.persist_flag: "use the flag this type is registered
 * with", falling back to persist-and-count for a type that is not registered.
 *
 * It is a sentinel rather than 0 because 0 is a real flag -- WFC_PERSIST_NONE,
 * a message that is delivered and kept nowhere -- and a send API where the
 * zero value silently means something else is a trap. Composing a message
 * from a registered type therefore reads:
 *
 *     wfc_content_out_t out = { .type = MY_TYPE,
 *                               .persist_flag = WFC_PERSIST_FROM_TYPE,
 *                               .searchable_content = "..." }; */
#define WFC_PERSIST_FROM_TYPE (-1)

typedef struct wfc_content_type wfc_content_type_t;

/* One line of this message, for a conversation row or a log.
 *
 * Runs on whichever task is storing or drawing -- the wfc task when a message
 * is filed, the UI task when a page repaints -- so it must not block and must
 * not call back into the client. `content` dies with the call (wfc_model.h),
 * which is why this writes into a buffer instead of returning a pointer.
 *
 * Always write something: an empty buffer is a blank conversation row. */
typedef void (*wfc_content_digest_fn)(const wfc_content_type_t *desc,
                                      const wfc_message_content_t *content,
                                      char *buf, size_t buf_size);

struct wfc_content_type {
    int32_t     type;
    /* What this type is called. Shown when there is nothing better to show,
     * and printed in logs, so it is worth making it readable. */
    const char *name;

    /* What wfc_send_message() puts on the wire when the caller asks for
     * WFC_PERSIST_FROM_TYPE. The receiving client honours it: a flag without
     * bit 0 is stored nowhere, and one without bit 1 counts towards nobody's
     * unread badge, so this is the field that decides whether a custom
     * message leaves a trace at the other end. */
    int32_t     persist_flag;

    /* Drawn as a centred line rather than a bubble: a tip, a group notice,
     * "X joined". The chat page asks this rather than testing type numbers,
     * so a custom notification looks like the built-in ones for free. */
    bool        notification;

    /* NULL means wfc_content_digest_body: the body if there is one, the name
     * if there is not. That is the right default for a custom type that
     * follows the convention every WFC client follows -- put the human
     * readable line in searchable_content -- and the reason a simple custom
     * message needs no code here at all. */
    wfc_content_digest_fn digest;
};

/* ------------------------------------------------------------ registering */

/* Adds or replaces one type. ESP_ERR_NO_MEM when the table is full (which is
 * a build-time bound, not a runtime condition -- the message says so and
 * names the constant to raise). */
esp_err_t wfc_register_content_type(const wfc_content_type_t *desc);

/* The array form, which is how an application's whole table registers in one
 * line. Stops and reports the first failure. */
esp_err_t wfc_register_content_types(const wfc_content_type_t *descs, size_t n);

/* The type's entry, or NULL if nothing knows this number. Registered types
 * are searched before built-in ones, so a registration wins. */
const wfc_content_type_t *wfc_content_type_find(int32_t type);

/* --------------------------------------------------------------- reading */

/* A short name for a content type, for logs. Never NULL; "unknown" for a
 * number nothing has registered.
 *
 * This used to format unknown types into a shared static buffer, which was
 * fine for the built-in types (there are no unknown ones) and a data race for
 * custom types (every one of them was unknown). The number belongs in the
 * digest, which has a buffer; a log line has this. */
const char *wfc_content_type_str(int32_t type);

/* True for a type that is drawn as a centred notice. False for anything
 * unregistered -- an unknown type is shown as an ordinary message, which is
 * the safer of the two guesses. */
bool wfc_content_is_notification(int32_t type);

/* What the conversation list shows for a message, and what a chat page draws
 * when it has no view of its own for the type. Always writes something.
 *
 * This is the one place the registry is consulted from inside the component:
 * the store calls it as a message is filed, so a conversation row carries the
 * digest of a message the store no longer has. That has a consequence worth
 * knowing -- a row shows the digest that was computed when it arrived, so
 * registering a type after its messages are already stored does not go back
 * and redraw history. Register at startup. */
void wfc_message_digest(const wfc_message_t *msg, char *buf, size_t buf_size);

/* ---------------------------------------------------- digest ingredients */

/* The default: searchable_content if the message has any, the type's name if
 * it does not. */
void wfc_content_digest_body(const wfc_content_type_t *desc,
                             const wfc_message_content_t *content,
                             char *buf, size_t buf_size);

/* The type's name, whatever the message carries. What the built-in media and
 * signalling types use -- an image's "digest" is the word image. */
void wfc_content_digest_name(const wfc_content_type_t *desc,
                             const wfc_message_content_t *content,
                             char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* WFC_CONTENT_H */
