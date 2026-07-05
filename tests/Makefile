CXX = g++
CXXFLAGS = -O2 -std=c++17 -Wall -g
LDFLAGS = $(shell pkg-config --libs --cflags protobuf) -lpthread
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
RPATH_PARENT = -Wl,-rpath,@loader_path/..
else
RPATH_PARENT = -Wl,-rpath,'$$ORIGIN/..'
endif

.PHONY: all

all: test_client_testserver

test_client_testserver: test_client_testserver.cpp
	$(CXX) $(CXXFLAGS) -I.. -o $@ $< -L.. $(RPATH_PARENT) -lutils $(LDFLAGS)

clean:
	-rm -f test_client_testserver

check: all
	python3 ./test_client_singlethreaded.py
	python3 ./test_client_multithreaded.py
	python3 ./test_server_singlethreaded.py
	python3 ./test_server_multithreaded.py
	python3 ./test_multithread_perf.py
