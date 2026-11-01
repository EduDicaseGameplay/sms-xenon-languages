# Host tests for the multiplayer networking (ext/multiplayer/net).
#   make run
CXX  ?= g++
CXXFLAGS ?= -O2 -g -std=gnu++17 -Wall -Wextra -pthread
WORK ?= $(abspath ../../../..)/build-multiplayer-test
NET := ../mp_net.cpp ../mp_socket.cpp
HDR := ../mp_net.h ../mp_socket.h ../mp_wire.h
$(WORK)/mp_net_test: mp_net_test.cc $(NET) $(HDR)
	mkdir -p $(WORK)
	$(CXX) $(CXXFLAGS) -I.. -o $@ mp_net_test.cc $(NET)
run: $(WORK)/mp_net_test
	$(WORK)/mp_net_test
clean:
	rm -rf $(WORK)
.PHONY: run clean
