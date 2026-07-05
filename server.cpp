#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include "utils.h"
#include "message.pb.h"

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
    struct Connection {
        uint32_t header = 0;
        size_t header_read = 0;
        std::string payload;
        size_t payload_read = 0;
        std::string response;
        size_t response_sent = 0;
    };

    enum class ReadResult { complete, pending, closed };

    static ReadResult read_available(int fd, char *buffer, size_t size, size_t &offset) {
        while (offset < size) {
            ssize_t received = recv(fd, buffer + offset, size - offset, 0);
            if (received == 0) {
                return ReadResult::closed;
            }
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return (errno == EAGAIN || errno == EWOULDBLOCK)
                           ? ReadResult::pending : ReadResult::closed;
            }
            offset += static_cast<size_t>(received);
        }
        return ReadResult::complete;
    }

    bool send_response(int fd, Connection &connection) {
        while (connection.response_sent < connection.response.size()) {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            ssize_t sent = send(fd, connection.response.data() + connection.response_sent,
                                connection.response.size() - connection.response_sent, flags);
            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return errno == EAGAIN || errno == EWOULDBLOCK;
            }
            if (sent == 0) {
                return false;
            }
            connection.response_sent += static_cast<size_t>(sent);
        }
        return false;  // Close only after the entire COUNTER response was sent.
    }

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
                short events = states_[fd].response.empty() ? POLLIN : POLLOUT;
                pollfds.push_back({fd, events, 0});
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

                if (entry.revents & (POLLIN | POLLOUT)) {
                    Connection &connection = states_[entry.fd];
                    bool keep = connection.response.empty()
                                    ? handle_message(entry.fd, connection)
                                    : send_response(entry.fd, connection);
                    if (!keep) {
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
                    states_.erase(fd);
                    close(fd);
                }
            }
        }
    }

    bool handle_message(int fd, Connection &connection) {
        ReadResult result = read_available(fd, reinterpret_cast<char *>(&connection.header),
                                           sizeof(connection.header), connection.header_read);
        if (result != ReadResult::complete) {
            return result == ReadResult::pending;
        }
        if (connection.payload.empty()) {
            uint32_t size = ntohl(connection.header);
            if (size == 0 || size > 64 * 1024 * 1024) {
                return false;
            }
            connection.payload.resize(size);
        }
        result = read_available(fd, connection.payload.data(), connection.payload.size(),
                                connection.payload_read);
        if (result != ReadResult::complete) {
            return result == ReadResult::pending;
        }

        sockets::message message;
        if (!message.ParseFromString(connection.payload) || !message.has_type()) {
            return false;
        }
        connection.header_read = 0;
        connection.payload_read = 0;
        connection.payload.clear();
        int64_t argument = message.has_argument() ? message.argument() : 0;

        switch (message.type()) {
            case OPERATION_ADD:
                number.fetch_add(argument, std::memory_order_relaxed);
                return true;
            case OPERATION_SUB:
                number.fetch_sub(argument, std::memory_order_relaxed);
                return true;
            case OPERATION_TERMINATION: {
                int64_t counter = number.load(std::memory_order_relaxed);
                print_counter(counter);
                sockets::message reply;
                reply.set_type(sockets::message::COUNTER);
                reply.set_argument(counter);
                std::string payload;
                if (!reply.SerializeToString(&payload)) {
                    return false;
                }
                uint32_t header = htonl(static_cast<uint32_t>(payload.size()));
                connection.response.assign(reinterpret_cast<const char *>(&header), sizeof(header));
                connection.response += payload;
                return send_response(fd, connection);
            }
            default:
                return false;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<int> connections_;
    std::unordered_map<int, Connection> states_;  // Accessed only by this worker.
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

        int flags = fcntl(connfd, F_GETFL, 0);
        if (flags == -1 || fcntl(connfd, F_SETFL, flags | O_NONBLOCK) == -1) {
            close(connfd);
            continue;
        }

        workers[next_worker % workers.size()].add_connection(connfd);
        ++next_worker;
    }

    close(listenfd);
    return 0;
}
