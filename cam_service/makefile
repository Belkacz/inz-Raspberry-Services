CC = gcc
CFLAGS = -Wall -Wextra -O2 -I.. -Ilibuvc/include -Iopenpnp-capture/include
LDFLAGS = -Llibuvc/lib -Lopenpnp-capture/lib
LIBS = -luvc -lopenpnp-capture -lwebsockets -lpthread -lm

TARGET = cam_service
SOURCES = cam_service.c ../common.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean