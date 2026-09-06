CXX      = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Isrc
LDFLAGS  = -pthread

SRCS     = src/app.cpp src/commands.cpp src/installer.cpp src/lockfile.cpp src/main.cpp src/utils.cpp
OBJS     = $(SRCS:.cpp=.o)

.PHONY: all windows linux clean

all: linux

windows: TARGET = lynx.exe
windows: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $^ $(LDFLAGS)

linux: TARGET = lynx
linux: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $^ $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(RC_OBJ) lynx lynx.exe