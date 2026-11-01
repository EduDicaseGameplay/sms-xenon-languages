# Host tests for the shared save file's merge rules and for painting players
# in their own colours (ext/multiplayer/sync).
#   make run
CXX  ?= g++
CXXFLAGS ?= -O2 -g -std=gnu++17 -Wall -Wextra
WORK ?= $(abspath ../../../..)/build-multiplayer-test
$(WORK)/mp_flags_test: mp_flags_test.cc ../mp_flags.cpp ../mp_flags.h ../../net/mp_wire.h
	mkdir -p $(WORK)
	$(CXX) $(CXXFLAGS) -I.. -o $@ mp_flags_test.cc ../mp_flags.cpp
$(WORK)/mp_look_test: mp_look_test.cc ../mp_look.cpp ../../mp_game_api.h ../../mp_recolour.h
	mkdir -p $(WORK)
	$(CXX) $(CXXFLAGS) -I.. -o $@ mp_look_test.cc ../mp_look.cpp
run: $(WORK)/mp_flags_test $(WORK)/mp_look_test
	$(WORK)/mp_flags_test
	$(WORK)/mp_look_test
clean:
	rm -rf $(WORK)
.PHONY: run clean
