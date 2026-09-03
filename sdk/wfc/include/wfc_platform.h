/* How this client identifies itself to the server. One place, on purpose.
 *
 * Decision D1 (ASSESSMENT.md 9.1, revised): we present as a **native** client,
 * not as the Web client. That is not cosmetic -- the server branches on it
 * twice, and both branches matter:
 *
 *   1. RouteRequest.platform == WEB or WX makes the server replace
 *      RouteResponse.long_port with its **WebSocket** port and hand back
 *      wss_port alongside it (server_commercial RouteHandler.java:167-183).
 *      A native platform gets the raw MQTT TCP port instead -- 1883 on a
 *      default deployment. Since this client speaks MQTT straight over TCP,
 *      claiming to be Web means dialling a WebSocket port with no HTTP
 *      upgrade, which the broker answers by silently closing the connection
 *      ten seconds later. So: the platform we report is the reason the port
 *      we are handed back is usable at all.
 *
 *   2. The WEB/WX branch also runs a Web license check, and WebSocket support
 *      is a professional-edition feature. The native path skips both.
 *
 * The `p` header is a separate switch on the same theme: "web" or "wx" (case
 * insensitive) makes the server base64 its /route **response**; anything else,
 * including absent, gets raw bytes (RouteAction.java:52-59). The request body
 * is always base64 either way. WFC_ROUTE_RESPONSE_BASE64 below has to agree
 * with WFC_PLATFORM_HEADER.
 *
 * These values move together -- platform 4 with a "web" `p` header is a
 * combination no released client sends. Sources: proto2's route request,
 * ProtoConstants.Platform, and the two server files named above.
 *
 * A token is issued FOR a platform, and the server keeps that on the session.
 * WFC_PLATFORM must be the platform the token was minted for.
 *
 * Side effect worth knowing: the server kicks same-platform sessions of the
 * same account off each other (MemorySessionStore.java:456). As OSX, this
 * board fights a macOS desktop client rather than a browser tab.
 */

#ifndef WFC_PLATFORM_H
#define WFC_PLATFORM_H

/* ProtoConstants.Platform.Platform_OSX. Not Platform_WEB (5) or Platform_WX
 * (6): those two are the WebSocket branch described above. */
#define WFC_PLATFORM        4
#define WFC_PLATFORM_HEADER "osx"   /* route request header `p` */

/* 0 unless WFC_PLATFORM_HEADER is "web" or "wx". */
#define WFC_ROUTE_RESPONSE_BASE64 0

#define WFC_APP_NAME    "cn.wildfirechat.chat"
#define WFC_DEVICE_NAME "esp32s3-box-3b"
#define WFC_PHONE_NAME  "esp32s3-box-3b"
#define WFC_APP_VERSION "0.1"
#define WFC_SDK_VERSION "0.1"

/* Sent as the appId/appKey route headers. The server only reads them on the
 * WEB/WX license path, so on the native path they are decoration -- kept
 * because they cost nothing and are needed the moment anyone flips the
 * platform back. Same constants WFC.js ships with (lib/connect/index.js:12). */
#define WFC_APP_ID  "web_12345678"
#define WFC_APP_KEY "6f8348670cb11cf434451bc9e7ba72eeaf3452c8"

#endif /* WFC_PLATFORM_H */
