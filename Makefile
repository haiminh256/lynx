CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Isrc
LDFLAGS  = -pthread
SRCS     = src/main.cpp src/utils.cpp src/install.cpp src/run.cpp src/uninstall.cpp src/create.cpp
OBJS     = $(SRCS:.cpp=.o)
TARGET   = lynx

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)