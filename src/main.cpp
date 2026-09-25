#include <cstring>
#include <iostream>

#include "usi_engine.h"
#include "version.h"

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::cout << shogi::kEngineName << " " << shogi::kEngineVersion << std::endl;
            return 0;
        }
    }

    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    shogi::UsiEngine engine;
    engine.loop();
    return 0;
}
