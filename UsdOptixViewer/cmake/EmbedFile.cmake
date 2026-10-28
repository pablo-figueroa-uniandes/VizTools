# Usage: cmake -DINPUT=file -DOUTPUT=file.cpp -DSYMBOL=name -P EmbedFile.cmake
# Writes a C++ source defining `extern const unsigned char SYMBOL[]` (NUL terminated)
# and `extern const size_t SYMBOL_size` (without the terminator).
file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hexLength)
math(EXPR size "${hexLength} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}"
  "#include <cstddef>\n"
  "extern const unsigned char ${SYMBOL}[] = {${bytes}0x00};\n"
  "extern const size_t ${SYMBOL}_size = ${size};\n")
