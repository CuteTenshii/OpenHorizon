// Lists, reads and extracts files through the engine's virtual file system,
// the way the game sees them.

#include "base/Log.h"
#include "vfs/Vfs.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

int usage()
{
    std::fprintf(stderr,
        "usage: fh1vfs DISC list FOLDER\n"
        "       fh1vfs DISC cat PATH\n"
        "       fh1vfs DISC extract PATH OUTFILE\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        return usage();
    }
    try {
        fh::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::string command = argv[2];
        if (command == "list") {
            for (const std::string& path : vfs.list(argv[3])) {
                std::printf("%s\n", path.c_str());
            }
            return 0;
        }
        if (command == "cat" || command == "extract") {
            const fh::Bytes data = vfs.readOrThrow(argv[3]);
            if (command == "cat") {
                std::fwrite(data.data(), 1, data.size(), stdout);
                return 0;
            }
            if (argc < 5) {
                return usage();
            }
            std::ofstream out(argv[4], std::ios::binary);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            return out ? 0 : 1;
        }
        return usage();
    } catch (const std::exception& e) {
        fh::logError("{}", e.what());
        return 1;
    }
}
