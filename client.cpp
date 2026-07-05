#include <iostream>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "utils.h"

namespace {

std::mutex print_mutex;

bool run_client_thread(const std::string &hostname, int port, int numMessages, int add,
                       int sub) {
    int sockfd = connect_socket(hostname.c_str(), port);
    if (sockfd == -1) {
        return false;
    }

    for (int i = 0; i < numMessages; ++i) {
        int32_t operation = (i % 2 == 0) ? OPERATION_ADD : OPERATION_SUB;
        int64_t argument = (operation == OPERATION_ADD) ? add : sub;
        if (send_msg(sockfd, operation, argument) != 0) {
            close(sockfd);
            return false;
        }
    }

    if (send_msg(sockfd, OPERATION_TERMINATION, 0) != 0) {
        close(sockfd);
        return false;
    }

    int32_t operation = 0;
    int64_t counter = 0;
    if (recv_msg(sockfd, &operation, &counter) != 0 || operation != OPERATION_COUNTER) {
        close(sockfd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cout << counter << '\n' << std::flush;
    }

    close(sockfd);
    return true;
}

}  // namespace

int main(int args, char *argv[]) {
    if (args < 7) {
        std::cerr << "usage: ./client <num_threads> <hostname> <port> "
                     "<num_messages> <add> <sub>\n";
        exit(1);
    }

    int numClients = std::atoi(argv[1]);
    std::string hostname = argv[2];
    int port = std::atoi(argv[3]);
    int numMessages = std::atoi(argv[4]);
    int add = std::atoi(argv[5]);
    int sub = std::atoi(argv[6]);

    if (numClients <= 0 || numMessages < 0) {
        std::cerr << "num_threads must be positive and num_messages non-negative\n";
        return 1;
    }

    std::vector<std::thread> threads;
    std::vector<unsigned char> results(static_cast<size_t>(numClients), false);
    threads.reserve(static_cast<size_t>(numClients));

    for (int i = 0; i < numClients; ++i) {
        threads.emplace_back([&, i]() {
            results[static_cast<size_t>(i)] =
                run_client_thread(hostname, port, numMessages, add, sub);
        });
    }

    bool ok = true;
    for (auto &thread : threads) {
        thread.join();
    }
    for (bool result : results) {
        ok = ok && result;
    }

    return ok ? 0 : 1;
}
