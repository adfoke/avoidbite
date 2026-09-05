CC      = cc
CFLAGS  = -Wall -Wextra -O2 -pthread
LDFLAGS = -pthread

TARGET  = avoidbite
SRC     = avoidbite.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
