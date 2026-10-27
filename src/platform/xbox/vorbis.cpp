// The Ogg Vorbis decoder for the CD music (third_party/stb_vorbis, public
// domain): decoding from memory only (audio.cpp loads each track).
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis/stb_vorbis.c"
