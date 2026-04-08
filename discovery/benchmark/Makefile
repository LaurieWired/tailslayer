CXX = g++
CXXFLAGS = -O2 -DNDEBUG -std=c++17 -pthread -D_GNU_SOURCE
LDFLAGS = -pthread

OBJS = app_config.o benchmark.o stats.o main.o

TARGET = hedged_read_cpp

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
