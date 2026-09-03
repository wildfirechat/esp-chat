/* pbc, wrapped.
 *
 * pbc reads and writes protobuf against a descriptor set loaded at runtime, so
 * there is no generated code: messages are addressed by field-name string.
 * The descriptor (components/pbc/pbdata.c, 13.5 KB in flash) covers all 137
 * message types of wfcmessage.proto, so adding a request later costs no build
 * step and a server-side field addition costs no client rebuild.
 *
 * These wrappers exist for the same reason proto2's pbbase.cc does: to keep
 * the split-integer and length-prefixed-string awkwardness of the pbc API in
 * one place.
 *
 * Lifetime rule, the one thing worth remembering: pbc decodes in place.
 * Strings and sub-messages read out of a wfc_pb_read_t point *into* the buffer
 * you passed to wfc_pb_read_begin(), so that buffer has to outlive the read.
 * Copy out (wfc_pb_get_str_copy) if you need the value later.
 */

#ifndef WFC_PB_H
#define WFC_PB_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pbc_env;
struct pbc_rmessage;
struct pbc_wmessage;

/* Registers the descriptor set. Idempotent; call once at startup. */
esp_err_t wfc_pb_init(void);

/* The shared environment, for code that wants the raw pbc API. */
struct pbc_env *wfc_pb_env(void);

/* ---------------------------------------------------------------- reading */

typedef struct pbc_rmessage wfc_pb_read_t;

/* `data` must stay valid and unmodified until wfc_pb_read_end(). */
wfc_pb_read_t *wfc_pb_read_begin(const char *type_name, const void *data, size_t len);
void           wfc_pb_read_end(wfc_pb_read_t *msg);

/* Missing fields read as 0 / "" / 0 entries, matching proto2's accessors. */
int32_t     wfc_pb_get_i32(wfc_pb_read_t *msg, const char *key, int index);
int64_t     wfc_pb_get_i64(wfc_pb_read_t *msg, const char *key, int index);
const char *wfc_pb_get_str(wfc_pb_read_t *msg, const char *key, int index, size_t *len);
int         wfc_pb_get_size(wfc_pb_read_t *msg, const char *key);
wfc_pb_read_t *wfc_pb_get_msg(wfc_pb_read_t *msg, const char *key, int index);

/* Copies a string field into `buf` and NUL-terminates it. Returns
 * ESP_ERR_INVALID_SIZE (and leaves buf empty) if the value does not fit. */
esp_err_t wfc_pb_get_str_copy(wfc_pb_read_t *msg, const char *key, int index,
                              char *buf, size_t buf_size);

/* ---------------------------------------------------------------- writing */

typedef struct pbc_wmessage wfc_pb_write_t;

wfc_pb_write_t *wfc_pb_write_begin(const char *type_name);
void            wfc_pb_write_abort(wfc_pb_write_t *msg);

esp_err_t wfc_pb_set_i32(wfc_pb_write_t *msg, const char *key, int32_t value);
esp_err_t wfc_pb_set_i64(wfc_pb_write_t *msg, const char *key, int64_t value);
esp_err_t wfc_pb_set_str(wfc_pb_write_t *msg, const char *key, const char *value);
esp_err_t wfc_pb_set_bytes(wfc_pb_write_t *msg, const char *key, const void *value, size_t len);

/* Appends a sub-message under `key` and returns a writer for it -- the way to
 * fill a repeated message field, called once per element.
 *
 * The sub-writer belongs to its parent: it is serialized into the parent and
 * released along with it, so never pass it to wfc_pb_write_end() or
 * wfc_pb_write_abort(). */
wfc_pb_write_t *wfc_pb_set_msg(wfc_pb_write_t *msg, const char *key);

/* Serializes, frees the writer either way. On success *out holds *out_len
 * bytes and is released with wfc_free(). */
esp_err_t wfc_pb_write_end(wfc_pb_write_t *msg, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* WFC_PB_H */
