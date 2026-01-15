# tests/Makefile
CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -g -I.. `pkg-config --cflags cmocka`
CXXFLAGS = -Wall -Wextra -g -std=c++17 -I.. `pkg-config --cflags opencv4`
LIBS = `pkg-config --libs opencv4 cmocka` -lpthread -lstdc++

TEST_TARGET = test_motion
TEST_SOURCES = test_motion.c
CPP_SOURCES = ../motion_detector.cpp
OBJECTS = test_motion.o motion_detector.o

all: $(TEST_TARGET)

$(TEST_TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LIBS) -o $(TEST_TARGET)

test_motion.o: test_motion.c
	$(CC) $(CFLAGS) -c $< -o $@

motion_detector.o: ../motion_detector.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(OBJECTS) $(TEST_TARGET)

.PHONY: all test clean