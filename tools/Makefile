CXX ?= c++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic

.PHONY: all clean
all: staticbus-replay

staticbus-replay: staticbus-replay.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f staticbus-replay
