CXX = g++
CXXFLAGS = -O3 -DNDEBUG -std=c++17 -pthread -D_GNU_SOURCE -Iinclude
LDFLAGS = -pthread

TARGET = tailslayer_example

all: $(TARGET)

$(TARGET): tailslayer_example.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
