# cmake -DIN=file -DOUT=header.h -DNAME=symbol -P embed.cmake
# Writes IN's bytes as `static const unsigned char NAME[]` (SPIR-V for gpu.c).
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n    " bytes "${bytes}")
file(WRITE "${OUT}" "/* made from ${IN} by tools/embed.cmake */\nstatic const unsigned char ${NAME}[] = {\n    ${bytes}\n};\n")
