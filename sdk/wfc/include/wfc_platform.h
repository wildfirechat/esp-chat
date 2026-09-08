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
 * Being native is also why /route sends no `p`, `appId` or `appKey` header.
 * Those three exist for the Web path only: `p` set to "web" or "wx" (case
 * insensitive) is what makes the server base64 its /route **response**, and
 * anything else -- including absent, which is our case -- gets raw bytes
 * (RouteAction.java:52-59). appId/appKey are forwarded into the license check
 * that the native branch never reaches. The request body is base64 either way.
 *
 * A token is issued FOR a platform, and the server keeps that on the session.
 * WFC_PLATFORM must be the platform the token was minted for.
 *
 * Side effect worth knowing: the server kicks same-platform sessions of the
 * same account off each other (MemorySessionStore.java:456). As AndroidWearable
 * this board fights another wearable client, not a phone or a desktop.
 */

#ifndef WFC_PLATFORM_H
#define WFC_PLATFORM_H

/* ProtoConstants.Platform.Platform_AndroidWearable. Not Platform_WEB (5) or
 * Platform_WX (6): those two are the WebSocket branch described above. */
#define WFC_PLATFORM 13

#define WFC_APP_NAME    "cn.wildfirechat.chat"
#define WFC_DEVICE_NAME "esp32s3-box-3b"
#define WFC_PHONE_NAME  "esp32s3-box-3b"
#define WFC_APP_VERSION "0.1"
#define WFC_SDK_VERSION "0.1"

#endif /* WFC_PLATFORM_H */
