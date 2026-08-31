#include "example_rekha.hpp"

#include <iostream>
#include <string_view>

namespace demo_log {
inline void info(std::string_view msg) {
    std::clog << "[info] " << msg << '\n';
}

inline void error(std::string_view prefix, std::string_view value) {
    std::cerr << "[error] " << prefix << value << '\n';
}
} // namespace demo_log

int main(int argc, char* argv[]) {
    const auto default_backend = []() -> std::string_view {
#if defined(KALPANA_ENABLE_SOKOL_BACKEND) && KALPANA_ENABLE_SOKOL_BACKEND
        return "sokol";
#elif defined(KALPANA_ENABLE_NOTCURSES_BACKEND) && KALPANA_ENABLE_NOTCURSES_BACKEND
        return "notcurses";
#else
        return "capture";
#endif
    };

    const auto print_usage = []() {
        demo_log::info("Usage: pebble_rekha_demo [capture|sokol|skol|notcurses]");
        demo_log::info("Default backend: sokol (if enabled), else notcurses, else capture");
        demo_log::info("       pebble_rekha_demo --help");
    };

    if (argc >= 2) {
        const std::string_view arg = argv[1];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
    }

    std::string_view backend = (argc >= 2) ? std::string_view{argv[1]} : default_backend();
    if (backend == "skol") {
        backend = "sokol";
    }
    if (backend != "capture" && backend != "sokol" && backend != "notcurses") {
        demo_log::error("Invalid backend: ", backend);
        print_usage();
        return 2;
    }

    return rekha_demo(backend);
}
