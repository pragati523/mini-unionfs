CC      = gcc
CFLAGS  = $(shell pkg-config fuse3 --cflags) -Wall -Wextra -g
LIBS    = $(shell pkg-config fuse3 --libs)
TARGET  = mini_unionfs
SRC     = src/mini_unionfs.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)
	rm -rf unionfs_test_env lower upper mnt

test: $(TARGET)
	bash scripts/test_unionfs.sh
