#include "utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "message.pb.h"

namespace {

constexpr uint32_t kMaxMessageSize = 64 * 1024 * 1024;

bool valid_port(int port) {
    return port >= 0 && port <= std::numeric_limits<uint16_t>::max();
}

void close_if_valid(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

void disable_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)fd;
#endif
}

bool recv_all(int sockfd, void *buffer, size_t size) {
    char *cursor = static_cast<char *>(buffer);
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t n = recv(sockfd, cursor, remaining, 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        cursor += n;
        remaining -= static_cast<size_t>(n);
    }

    return true;
}

bool send_all(int sockfd, const void *buffer, size_t size) {
    const char *cursor = static_cast<const char *>(buffer);
    size_t remaining = size;

    while (remaining > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(sockfd, cursor, remaining, MSG_NOSIGNAL);
#else
        ssize_t n = send(sockfd, cursor, remaining, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }

        cursor += n;
        remaining -= static_cast<size_t>(n);
    }

    return true;
}

}  // namespace

extern "C" {

int listening_socket(int port) {
    if (!valid_port(port)) {
        return -1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    int enabled = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    disable_sigpipe(sockfd);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(sockfd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int connect_socket(const char *hostname, const int port) {
    if (hostname == nullptr || !valid_port(port)) {
        return -1;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *results = nullptr;
    std::string service = std::to_string(port);
    if (getaddrinfo(hostname, service.c_str(), &hints, &results) != 0) {
        return -1;
    }

    int sockfd = -1;
    for (addrinfo *entry = results; entry != nullptr; entry = entry->ai_next) {
        sockfd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (sockfd < 0) {
            continue;
        }

        disable_sigpipe(sockfd);
        if (connect(sockfd, entry->ai_addr, entry->ai_addrlen) == 0) {
            break;
        }

        close_if_valid(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(results);
    return sockfd;
}

int accept_connection(int sockfd) {
    while (true) {
        int connfd = accept(sockfd, nullptr, nullptr);
        if (connfd >= 0) {
            disable_sigpipe(connfd);
            return connfd;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

}

int recv_msg(int sockfd, int32_t *operation_type, int64_t *argument) {
    if (operation_type == nullptr || argument == nullptr) {
        return 1;
    }

    uint32_t network_size = 0;
    if (!recv_all(sockfd, &network_size, sizeof(network_size))) {
        return 1;
    }

    uint32_t payload_size = ntohl(network_size);
    if (payload_size == 0 || payload_size > kMaxMessageSize) {
        return 1;
    }

    std::vector<char> payload(payload_size);
    if (!recv_all(sockfd, payload.data(), payload.size())) {
        return 1;
    }

    sockets::message message;
    if (!message.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return 1;
    }
    if (!message.has_type() || !sockets::message::OperationType_IsValid(message.type())) {
        return 1;
    }

    *operation_type = static_cast<int32_t>(message.type());
    *argument = message.has_argument() ? message.argument() : 0;
    return 0;

}

int send_msg(int sockfd, int32_t operation_type, int64_t argument) {
    if (!sockets::message::OperationType_IsValid(operation_type)) {
        return 1;
    }

    sockets::message message;
    message.set_type(static_cast<sockets::message::OperationType>(operation_type));
    message.set_argument(argument);

    std::string payload;
    if (!message.SerializeToString(&payload) || payload.size() > kMaxMessageSize) {
        return 1;
    }

    uint32_t network_size = htonl(static_cast<uint32_t>(payload.size()));
    if (!send_all(sockfd, &network_size, sizeof(network_size))) {
        return 1;
    }
    if (!send_all(sockfd, payload.data(), payload.size())) {
        return 1;
    }

    return 0;

}

}
