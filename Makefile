CC = gcc
CFLAGS = `pkg-config --cflags sdl2`
LDFLAGS = `pkg-config --libs sdl2` -lv4l2

all: circam

circam: circam.c
	$(CC) -o circam circam.c $(CFLAGS) $(LDFLAGS)
	
clean:
	rm -f circam
