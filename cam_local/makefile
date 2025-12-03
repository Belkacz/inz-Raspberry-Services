CC = g++
CXX = g++
CFLAGS = -Wall -Wextra -O2 -I..
CXXFLAGS = -Wall -Wextra -O2 -std=c++11 -I.. `pkg-config --cflags opencv4`
LDFLAGS = 
LIBS = `pkg-config --libs opencv4 libuvc libwebsockets` -lpthread -lm

TARGET = cam_service
C_SOURCES = motion_mix.c ../common.c
CPP_SOURCES = motion_detector.cpp
C_OBJECTS = $(C_SOURCES:.c=.o)
CPP_OBJECTS = $(CPP_SOURCES:.cpp=.o)
OBJECTS = $(C_OBJECTS) $(CPP_OBJECTS)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

# Kompiluj .c przez g++ (nie gcc!)
%.o: %.c
	$(CXX) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean