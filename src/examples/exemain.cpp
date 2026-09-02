#include "utils/log.hpp"
#include "example_rekha.hpp"

#include <string_view>

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::string_view{argv[1]} == "rekha") {
        const std::string_view backend = (argc >= 3) ? std::string_view{argv[2]} : std::string_view{"capture"};
        return rekha_demo(backend);
    }

    lg::info("Usage: pebble rekha [capture|sokol|notcurses]");
    return 0;
}