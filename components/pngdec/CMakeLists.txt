idf_component_register(
    SRCS "src/PNGdec.cpp" "src/adler32.c" "src/crc32.c" "src/infback.c"
         "src/inffast.c" "src/inflate.c" "src/inftrees.c" "src/zutil.c"
    INCLUDE_DIRS "include" "src"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=old-style-definition
    -Wno-error=implicit-fallthrough
)
