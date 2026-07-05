#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <errno.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "utils.h"

std::atomic<int64_t> number{0};

namespace {

std::mutex print_mutex;

void print_counter(int64_t value) {
    std::lock_guard<std::mutex> lock(print_mutex);
    std::cout << value << '\n' << std::flush;
}

class Worker {
   public:
    void start() {
        thread_ = std::thread(&Worker::run, this);
    }

    void add_connection(int fd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_.push_back(fd);
        }
        cv_.notify_one();
    }

   private:
    void run() {
        while (true) {
            std::vector<int> snapshot;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return !connections_.empty(); });
                snapshot = connections_;
            }

            std::vector<pollfd> pollfds;
            pollfds.reserve(snapshot.size());
            for (int fd : snapshot) {
                pollfds.push_back({fd, POLLIN, 0});
            }

            int ready = poll(pollfds.data(), static_cast<nfds_t>(pollfds.size()), 100);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ready == 0) {
                continue;
            }

            std::vector<int> closed;
            for (pollfd &entry : pollfds) {
                if (entry.revents == 0) {
                    continue;
                }

                if (entry.revents & POLLIN) {
                    if (!handle_message(entry.fd)) {
                        closed.push_back(entry.fd);
                    }
                    continue;
                }

                if (entry.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    closed.push_back(entry.fd);
                }
            }

            if (!closed.empty()) {
                std::sort(closed.begin(), closed.end());
                closed.erase(std::unique(closed.begin(), closed.end()), closed.end());

                std::lock_guard<std::mutex> lock(mutex_);
                connections_.erase(
                    std::remove_if(connections_.begin(), connections_.end(),
                                   [&](int fd) {
                                       return std::find(closed.begin(), closed.end(), fd) !=
                                              closed.end();
                                   }),
                    connections_.end());
                for (int fd : closed) {
                    close(fd);
                }
            }
        }
    }

    bool handle_message(int fd) {
        int32_t operation = 0;
        int64_t argument = 0;
        if (recv_msg(fd, &operation, &argument) != 0) {
            return false;
        }

        switch (operation) {
            case OPERATION_ADD:
                number.fetch_add(argument, std::memory_order_relaxed);
                return true;
            case OPERATION_SUB:
                number.fetch_sub(argument, std::memory_order_relaxed);
                return true;
            case OPERATION_TERMINATION: {
                int64_t counter = number.load(std::memory_order_relaxed);
                print_counter(counter);
                send_msg(fd, OPERATION_COUNTER, counter);
                return false;
            }
            default:
                return false;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<int> connections_;
    std::thread thread_;
};

}  // namespace

int main(int args, char *argv[]) {
    if (args < 3) {
        std::cerr << "usage: ./server <numThreads> <port>\n";
        exit(1);
    }

    int numThreads = std::atoi(argv[1]);
    int port = std::atoi(argv[2]);

    if (numThreads <= 0) {
        std::cerr << "numThreads must be positive\n";
        return 1;
    }

    int listenfd = listening_socket(port);
    if (listenfd == -1) {
        std::cerr << "failed to create listening socket\n";
        return 1;
    }

    std::vector<Worker> workers(static_cast<size_t>(numThreads));
    for (Worker &worker : workers) {
        worker.start();
    }

    size_t next_worker = 0;
    while (true) {
        int connfd = accept_connection(listenfd);
        if (connfd == -1) {
            continue;
        }

        workers[next_worker % workers.size()].add_connection(connfd);
        ++next_worker;
    }

    close(listenfd);
    return 0;
}
