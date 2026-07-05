# Set you prefererred CFLAGS/compiler compiler here.
# Our github runner provides gcc-10 by default.
CC ?= cc
CFLAGS ?= -g -Wall -O2
CXX ?= c++
CXXFLAGS ?= -g -Wall -O2 -std=c++17
CARGO ?= cargo
RUSTFLAGS ?= -g
LDFLAGS = $(shell pkg-config --libs --cflags protobuf) -lpthread
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SHARED_FLAGS = -shared -fPIC -Wl,-install_name,@rpath/libutils.so
RPATH_SELF = -Wl,-rpath,@loader_path
else
SHARED_FLAGS = -shared -fPIC
RPATH_SELF = -Wl,-rpath,'$$ORIGIN'
endif

.PHONY: all clean

all: libutils.so server client

clean:
	-rm -f server client libutils.so message.pb.*

# --- C++ build steps ---

message.pb.cc: message.proto
	protoc --cpp_out=. $^

libutils.so: utils.cpp message.pb.cc
	$(CXX) $(CXXFLAGS) $(SHARED_FLAGS) -o $@ utils.cpp message.pb.cc $(LDFLAGS)

server: server.cpp libutils.so
	$(CXX) $(CXXFLAGS) -o $@ server.cpp -L. $(RPATH_SELF) -lutils -lpthread

client: client.cpp libutils.so
	$(CXX) $(CXXFLAGS) -o $@ client.cpp -L. $(RPATH_SELF) -lutils -lpthread

# --- Rust build steps ---

# libutils.so:
# 	cargo build --lib && cp target/debug/libutils.so .

# server:
# 	cargo build --bin server && cp target/debug/server .

# client:
# 	cargo build --bin client && cp target/debug/client .

# Usually there is no need to modify this
check: all
	$(MAKE) -C tests check
