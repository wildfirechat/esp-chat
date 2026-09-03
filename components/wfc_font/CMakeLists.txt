# The generated font is one enormous const array, so it compiles slowly and
# changes never. It is its own component to keep it out of main/'s rebuilds
# and to keep the licence next to the artefact it applies to.
idf_component_register(SRCS "src/wfc_font_16.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl)
