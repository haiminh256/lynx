CXX      = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Isrc
LDFLAGS  = -pthread
SRCS     = src/app.cpp src/commands.cpp src/installer.cpp src/lockfile.cpp src/main.cpp src/utils.cpp
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

