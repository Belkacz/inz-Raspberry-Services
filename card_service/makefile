CC = gcc
CFLAGS = -Wall -Wextra -O2 -I..
LIBS = -lwebsockets -lpthread

TARGET = card_service
SOURCES = card_service.c card_service_fn.c ../common.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
