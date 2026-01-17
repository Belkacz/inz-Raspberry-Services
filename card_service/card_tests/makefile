CC = gcc
CFLAGS = -Wall -Wextra -g -I.. -I../../
LIBS = -lcmocka
TARGET = card_tests
SOURCES = test_card.c ../card_service_fn.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LIBS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all test clean