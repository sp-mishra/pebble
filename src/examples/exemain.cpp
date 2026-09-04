#include "utils/log.hpp"
// #include "example_rekha.hpp"
#include "example_sanchaya.hpp"

#include <string_view>

int main(int argc, char* argv[]) {
    // if (argc >= 2 && std::string_view{argv[1]} == "rekha") {
    //     const std::string_view backend = (argc >= 3) ? std::string_view{argv[2]} : std::string_view{"capture"};
    //     return rekha_demo(backend);
    // }

    sanchaya::example::run_sanchaya_demo();
    // if (argc >= 2 && std::string_view{argv[1]} == "sanchaya") {
    //     return sanchaya::example::run_sanchaya_demo();
    // }

    lg::info("Usage: pebble [rekha [capture|sokol|notcurses] | sanchaya]");
    return 0;
}