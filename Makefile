CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
TARGET = vm
SRC = vm.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) correct.txt

.PHONY: all clean
