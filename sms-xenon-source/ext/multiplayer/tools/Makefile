# The multiplayer test bot (mp_bot.cpp).
#   make            builds build-multiplayer-test/mp_bot
CXX  ?= g++
CXXFLAGS ?= -O2 -g -std=gnu++11 -Wall -Wextra -pthread
WORK ?= $(abspath ../../..)/build-multiplayer-test
SRC := mp_bot.cpp ../net/mp_net.cpp ../net/mp_socket.cpp ../sync/mp_flags.cpp ../sync/mp_controller.cpp
BUILD := $(shell git -C ../../.. rev-parse HEAD 2>/dev/null | cut -c1-12)
$(WORK)/mp_bot: $(SRC) $(wildcard ../*.h ../net/*.h ../sync/*.h)
	mkdir -p $(WORK)
	$(CXX) $(CXXFLAGS) -DSMS_GAME_BUILD='"$(or $(SMS_GAME_BUILD),$(BUILD))"' -o $@ $(SRC)
.PHONY: clean
clean:
	rm -f $(WORK)/mp_bot
