# app_*.c is the application proper: what the board knows about itself
# (app_cfg.c), how it gets on a network (app_net.c) and into an account
# (app_login.c). ui_provision.c and ui_login.c are those two as pages.
# ui.c is the shell; each ui_*.c after it is one page (see ui_page.h).
# ui_lv_mem.c is not a page -- it is LVGL's allocator, moved to PSRAM.
# ui_msg_view.c is not a page either -- it is the table saying which content
# type is drawn by which view, and each ui_msg_*.c after it is one such view
# (see ui_msg_view.h).
# ui_pinyin_dict.c is generated -- tools/make_pinyin.py.
# wfc_custom_message/ is this deployment's own message types, kept in a
# directory of its own so a fork of it can be dropped in whole -- the same
# arrangement the web client's src/wfc_custom_message/ has.
set(app_srcs "app_main.c"
             "app_cfg.c"
             "app_net.c"
             "app_login.c"
             "ui.c"
             "ui_lv_mem.c"
             "ui_convs.c"
             "ui_contacts.c"
             "ui_contact.c"
             "ui_requests.c"
             "ui_group.c"
             "ui_pick.c"
             "ui_chat.c"
             "ui_msg_view.c"
             "ui_msg_image.c"
             "ui_msg_voice.c"
             "ui_media.c"
             "ui_voice.c"
             "app_audio.c"
             "ui_compose.c"
             "ui_record.c"
             "ui_status.c"
             "ui_provision.c"
             "ui_login.c"
             "ui_log.c"
             "ui_pinyin_dict.c"
             "wfc_custom_message/custom_message_config.c"
             "wfc_custom_message/custom_message_view.c")

set(app_requires wfc wfc_font nvs_flash esp_wifi esp_netif
                 esp_event esp_hw_support esp_timer
                 esp_http_client esp-tls espressif__esp_jpeg
                 # Provisioning and the QR login: an access point with three
                 # JSON endpoints on it (app_net.c), and two more requests
                 # against the app server (app_login.c). cJSON is already in
                 # the image in a build with calls -- the AV SDK parses SDP
                 # with it -- and costs about 25 KB in one without.
                 esp_http_server espressif__cjson
                 # Voice messages: the board's two codecs and AMR-NB. Named
                 # here and not under CONFIG_APP_CALL -- a voice message needs
                 # the microphone, not the AV SDK, so it is in every build.
                 espressif__esp-box-3 espressif__esp_codec_dev
                 espressif__esp_audio_codec)

# Voice calls, the one feature that can be left out. Same shape as the client's
# two store backends (wfc-esp/components/wfc/CMakeLists.txt): one header, two
# implementations, exactly one compiled -- so no file in this directory has an
# #ifdef in it and only ui_call.c ever includes wfav.h.
#
# The two lists below are decided by two different questions, and that is not
# an oversight:
#
#   REQUIRES is read in an early CMake pass that runs before sdkconfig exists,
#   in a separate process that inherits none of this -- a CONFIG_ test there
#   silently reads as false (the same trap components/wfc/CMakeLists.txt
#   documents). So the question it can answer is one about the filesystem: is
#   there an AV SDK to build against at all? Either form counts -- the sibling
#   source checkout, or the packaged ../sdk/wfav that the published tree has
#   instead (see the root CMakeLists.txt).
#
#   SRCS is evaluated in the real pass, with sdkconfig loaded, so that one is
#   the actual switch.
#
# Which leaves the combination worth naming: sdkconfig says yes and there is no
# SDK in either form. That is a configuration mistake rather than a trim, so it
# stops the build with a sentence instead of a missing header.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../wfav-esp/components/wfav" OR
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/../sdk/wfav")
    list(APPEND app_requires wfav)
elseif(CONFIG_APP_CALL)
    message(FATAL_ERROR
        "CONFIG_APP_CALL is on but there is no AV SDK to build against: neither "
        "../wfav-esp (source, beside this project) nor sdk/wfav (package). "
        "Provide one, or put CONFIG_APP_CALL=n in sdkconfig.local, delete "
        "sdkconfig and build again.")
endif()

if(CONFIG_APP_CALL)
    list(APPEND app_srcs "ui_call.c")
else()
    list(APPEND app_srcs "ui_call_none.c")
endif()

# Push-to-talk, the second feature that can be left out, and it is left out
# INDEPENDENTLY of calls: a talk is AMR-NB over the long link, so it wants the
# microphone and nothing from the AV stack. Same two questions as above, asked
# in the same two places for the same reasons -- REQUIRES cannot see sdkconfig,
# so it asks the filesystem; SRCS can, so it is the switch.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../wfptt-esp/components/wfptt" OR
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/../sdk/wfptt")
    list(APPEND app_requires wfptt)
elseif(CONFIG_APP_PTT)
    message(FATAL_ERROR
        "CONFIG_APP_PTT is on but there is no push-to-talk SDK to build against: "
        "neither ../wfptt-esp (source, beside this project) nor sdk/wfptt "
        "(package). Provide one, or put CONFIG_APP_PTT=n in sdkconfig.local, "
        "delete sdkconfig and build again.")
endif()

if(CONFIG_APP_PTT)
    list(APPEND app_srcs "app_ptt.c" "ui_ptt.c")
else()
    list(APPEND app_srcs "ui_ptt_none.c")
endif()

idf_component_register(SRCS ${app_srcs}
                       INCLUDE_DIRS "." "wfc_custom_message"
                       REQUIRES ${app_requires})

# ui_lv_mem.c defines lv_malloc_core() and friends, which only LVGL calls.
# liblvgl__lvgl.a is listed ahead of libmain.a and the two are not inside a
# linker group, so nothing ever pulls that object out of the archive and the
# link fails on eight undefined references. One -u is enough: resolving any
# symbol in the object brings in the whole object.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
