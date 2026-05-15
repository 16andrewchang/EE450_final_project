.PHONY: all clean

CXX = g++

CXXFLAGS = -g -Wall -Wextra -std=c++17
LDLIBS   = -pthread

TARGETS = client monitor serverA serverB serverC serverM

all: $(TARGETS)

%: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) $(TARGETS)
