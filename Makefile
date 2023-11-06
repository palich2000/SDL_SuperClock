CC=gcc
CCFLAGS=$(shell pkg-config --cflags sdl2) -ggdb3 -O0 --std=c99 -Wall -Wextra -Wwrite-strings -Werror -Wfatal-errors
LDFLAGS=$(shell pkg-config --libs sdl2) -lSDL2_image -lSDL2_ttf -lSDL2main -lpthread -ljson-c -lzip -lmosquitto
TESTFLAGS=-fsanitize=leak -fsanitize=address -fsanitize=undefined
TARGET=superclock-sdl
SOURCES=*.c


all:	$(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CCFLAGS) $(SOURCES) $(LDFLAGS) -o $(TARGET)

clean:
	rm -rf $(TARGET)

rebuild:
	$(clean)
	$(CC) $(LDFLAGS) $(CCFLAGS) $(SOURCES) -o $(TARGET)

test:
	$(clean)
	$(CC) $(LDFLAGS) $(CCFLAGS) $(TESTFLAGS) $(SOURCES) -o $(TARGET)
install: $(TARGET)
	install $(TARGET) ~/bin/
	install ./images/*.png ~/bin/