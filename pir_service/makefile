CC = gcc
CFLAGS = -Wall -Wextra -O2 -I..
LIBS = -lwebsockets -lgpiod -lpthread

TARGET = pir_service
SOURCES = pir_service.c ../common.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean