/* This deployment's message type numbers.
 *
 * Below 1000 belongs to WFC (wfc_model.h has the ones this client knows); an
 * application's own types start at 1000. The number is the whole identity of
 * a message type on the wire -- every client that does not recognise it shows
 * it as an unknown message and stores it if the persist flag says to -- so
 * they are worth keeping in one short file that can be read next to the web
 * client's customMessageContentType.js and diffed by eye.
 */

#ifndef CUSTOM_MESSAGE_TYPE_H
#define CUSTOM_MESSAGE_TYPE_H

#define MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST              1001
#define MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST_NOTIFICATION 1002

#endif /* CUSTOM_MESSAGE_TYPE_H */
