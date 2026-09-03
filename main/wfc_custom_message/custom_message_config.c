/* The table: every message type this deployment adds, and what the client
 * should do with it. customMessageConfig.js, in C.
 *
 * A type is four facts -- its number, its name, its persist flag, whether it
 * draws as a notice -- plus, when the default is not right, a function that
 * turns one into a line of text. Register them and the client treats them
 * like its own: the conversation list shows a proper digest, the unread badge
 * counts what should count, a reboot brings them back out of the store.
 *
 * Two things are worth knowing before adding one.
 *
 * The persist flag is the whole of the far end's behaviour. It travels with
 * the message and every WFC client honours it: without bit 0 nobody stores
 * the message, without bit 1 nobody counts it. A type that says
 * WFC_PERSIST_PERSIST_COUNT here is a message; one that says
 * WFC_PERSIST_TRANSPARENT is a signal that leaves no trace anywhere.
 *
 * Where the body goes decides how much code the type needs. Put a readable
 * line in searchable_content -- the convention every WFC client follows --
 * and the digest is free, which is the whole of MY_TEST below. Put it
 * somewhere else and the type owes the table a digest function, which is the
 * whole of MY_TEST_NOTIFICATION.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "custom_message.h"

static const char *TAG = "custom_msg";

/* ------------------------------------------------------------- digests */

/* The notification example carries its tip in MessageContent.content, the
 * same field the web version's encode() writes, so the default digest (the
 * body in searchable_content) would find nothing and fall back to the type's
 * name. Three lines fix that, and they are the reason the hook exists. */
static void digest_test_notification(const wfc_content_type_t *desc,
                                     const wfc_message_content_t *content,
                                     char *buf, size_t buf_size)
{
    if (content->content != NULL && content->content[0] != '\0') {
        wfc_copy_text(buf, buf_size, content->content);
        return;
    }
    wfc_content_digest_name(desc, content, buf, buf_size);
}

/* ---------------------------------------------------------- the table */

static const wfc_content_type_t CUSTOM_MESSAGES[] = {
    {
        .type         = MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST,
        .name         = "自定义消息",
        .persist_flag = WFC_PERSIST_PERSIST_COUNT,
        .notification = false,
        /* NULL: the body is in searchable_content, where the default digest
         * looks. A custom type that follows the convention needs no code. */
        .digest       = NULL,
    },
    {
        .type         = MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST_NOTIFICATION,
        .name         = "自定义通知",
        /* Persist without count: a notice belongs in the history and does not
         * belong in a badge, which is what the built-in group notices do. */
        .persist_flag = WFC_PERSIST_PERSIST,
        .notification = true,
        .digest       = digest_test_notification,
    },
};

/* The built-in types, renamed.
 *
 * A registration replaces a built-in entry, and the component ships ASCII
 * labels ("image", "call") because the words a person reads are the
 * application's business, not the protocol's. This is where they become
 * Chinese -- the conversation list says 「[图片]」 rather than "image" -- and
 * it is the same mechanism a custom type uses, not a special case for us.
 *
 * Only the name changes; the flags stay as the protocol defines them. */
static const wfc_content_type_t RENAMED_BUILTINS[] = {
    { WFC_CONTENT_VOICE,      "[语音]",   WFC_PERSIST_PERSIST_COUNT, false,
      wfc_content_digest_name },
    { WFC_CONTENT_IMAGE,      "[图片]",   WFC_PERSIST_PERSIST_COUNT, false,
      wfc_content_digest_name },
    { WFC_CONTENT_LOCATION,   "[位置]",   WFC_PERSIST_PERSIST_COUNT, false,
      wfc_content_digest_name },
    { WFC_CONTENT_FILE,       "[文件]",   WFC_PERSIST_PERSIST_COUNT, false,
      wfc_content_digest_name },
    { WFC_CONTENT_VIDEO,      "[视频]",   WFC_PERSIST_PERSIST_COUNT, false,
      wfc_content_digest_name },
    { WFC_CONTENT_STICKER,    "[表情]",   WFC_PERSIST_PERSIST_COUNT, false,
      wfc_content_digest_name },
    { WFC_CONTENT_LINK,       "[链接]",   WFC_PERSIST_PERSIST_COUNT, false,
      wfc_content_digest_name },
    { WFC_CONTENT_USER_CARD,  "[名片]",   WFC_PERSIST_PERSIST_COUNT, false,
      wfc_content_digest_name },
    { WFC_CONTENT_RECALL_NOTIFY, "撤回了一条消息", WFC_PERSIST_PERSIST, true,
      wfc_content_digest_name },
    { WFC_CONTENT_VOIP_START, "[通话]",   WFC_PERSIST_PERSIST_COUNT, false,
      wfc_content_digest_name },
};

void custom_messages_register(void)
{
    esp_err_t err = wfc_register_content_types(
        CUSTOM_MESSAGES, sizeof(CUSTOM_MESSAGES) / sizeof(CUSTOM_MESSAGES[0]));

    if (err == ESP_OK) {
        err = wfc_register_content_types(
            RENAMED_BUILTINS, sizeof(RENAMED_BUILTINS) / sizeof(RENAMED_BUILTINS[0]));
    }

    /* Not fatal, and not silent: a board that boots with an unregistered type
     * still receives and stores those messages, it just draws them as
     * "type 1001" until someone reads the log and raises REGISTERED_MAX. */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "content types not registered: %s", esp_err_to_name(err));
    }
}

/* --------------------------------------------------------------- sending */

/* Both of these are what the web client's encode() is: they decide which
 * field the body goes in, and that decision has to match what decode() --
 * here, the digest above and custom_message_text() in custom_message_view.c
 * -- reads back out. Encode and decode are one pair; keeping them in one
 * directory is how they stay in step.
 *
 * WFC_PERSIST_FROM_TYPE rather than a literal: the flag lives in the table
 * above, so a type whose storage rules change changes in one place. */
esp_err_t custom_message_send_test(const wfc_conversation_t *conv, const char *text)
{
    if (conv == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wfc_content_out_t content = {
        .type               = MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST,
        .persist_flag       = WFC_PERSIST_FROM_TYPE,
        /* Where every WFC client looks for something readable, which is what
         * makes this message legible on a phone that has never heard of type
         * 1001: it shows up as text rather than as an unsupported message. */
        .searchable_content = text,
        .push_content       = text,
    };

    return wfc_send_message(conv, &content, NULL, 0, NULL, NULL);
}

esp_err_t custom_message_send_test_notification(const wfc_conversation_t *conv,
                                                const char *tip)
{
    if (conv == NULL || tip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wfc_content_out_t content = {
        .type         = MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST_NOTIFICATION,
        .persist_flag = WFC_PERSIST_FROM_TYPE,
        /* MessageContent.content, matching the web version's encode(). */
        .content      = tip,
    };

    return wfc_send_message(conv, &content, NULL, 0, NULL, NULL);
}
