// main.cpp — dsd-server entry point.
//
// Usage: dsd-server [listen_address] [port] [threads]
//   defaults: 0.0.0.0 22600 4

#include "session.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <cstdlib>

int main(int argc, char** argv) {
    using namespace dsdsrv;

    std::string address_str = argc > 1 ? argv[1] : "0.0.0.0";
    unsigned short port = argc > 2 ? static_cast<unsigned short>(std::atoi(argv[2])) : 22600;
    int threads = argc > 3 ? std::atoi(argv[3]) : 4;
    if (threads < 1) threads = 1;

    net::io_context ioc{threads};

    auto address = net::ip::make_address(address_str);
    Server server(ioc, tcp::endpoint{address, port});

    std::cerr << "dsd-server listening on " << address_str << ":" << port
              << " with " << threads << " threads\n";

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads - 1));
    for (int i = 0; i < threads - 1; ++i) {
        pool.emplace_back([&ioc] { ioc.run(); });
    }
    ioc.run();

    for (auto& t : pool) t.join();
    return 0;
}
