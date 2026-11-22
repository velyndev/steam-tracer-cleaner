// svg2rgba: renders an SVG at one or more square pixel sizes and writes raw 8-bit RGBA with
// straight alpha, top-down rows, one file per size: <out_prefix><size>.rgba
//
//   svg2rgba <input.svg> <out_prefix> <size> [<size> ...]
//
// Used by tools/make_icon.py; see tools/icon/CMakeLists.txt for how to build it.

#include <lunasvg.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: svg2rgba <input.svg> <out_prefix> <size> [<size> ...]\n");
        return 2;
    }
    auto document = lunasvg::Document::loadFromFile(argv[1]);
    if (!document) {
        std::fprintf(stderr, "svg2rgba: cannot load %s\n", argv[1]);
        return 1;
    }
    for (int i = 3; i < argc; ++i) {
        const int size = std::atoi(argv[i]);
        if (size <= 0 || size > 4096) {
            std::fprintf(stderr, "svg2rgba: bad size %s\n", argv[i]);
            return 2;
        }
        lunasvg::Bitmap bitmap = document->renderToBitmap(size, size, 0x00000000);
        if (bitmap.isNull()) {
            std::fprintf(stderr, "svg2rgba: render failed at %d\n", size);
            return 1;
        }
        bitmap.convertToRGBA();  // premultiplied BGRA in memory to straight RGBA
        const std::string path = std::string(argv[2]) + std::to_string(size) + ".rgba";
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            std::fprintf(stderr, "svg2rgba: cannot write %s\n", path.c_str());
            return 1;
        }
        const unsigned char* row = bitmap.data();
        for (int y = 0; y < size; ++y, row += bitmap.stride()) {
            std::fwrite(row, 1, static_cast<std::size_t>(size) * 4U, file);
        }
        std::fclose(file);
    }
    return 0;
}
