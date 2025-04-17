# Circam
A simple circular webcam viewer for Linux using SDL2 and V4L2. Displays a webcam feed in a resizable, draggable, circular window with a centered square crop, good for screencast desktop sharing.

## Features
- Circular window with a square-cropped webcam feed.
- Resize via mouse wheel, keyboard (`+`/`-`), or window manager.
- Drag to move with left-click.
- Optional always-on-top mode (`-t`).
- Custom initial size (`-s <size>`).
- Lightweight and efficient, using hardware-accelerated rendering.

## Installation
### Prerequisites
- **SDL2**: `libsdl2-dev`
- **V4L2**: `libv4l-dev`
- A webcam supporting YUYV format (most webcams).

On Linux Mint/Ubuntu:

	sudo apt update
	sudo apt install libsdl2-dev libv4l-dev

# Build

	git clone https://github.com/b1tfl0w/circam.git
	cd circam
	make

# Usage

./circam [-t] [-s <size>] <video_device>

-t: Enable always-on-top.

-s <size>: Set initial window size (minimum 100 pixels).

<video_device>: Webcam device (e.g., /dev/video0).

# Example

	./circam -t -s 256 /dev/video0

## Controls

- Exit: Press Esc or close the window.

- Move: Left-click and drag.

### Resize:

- Mouse Wheel: Scroll up to increase size, down to decrease (10-pixel steps).

- Keyboard: + or = to increase, - to decrease (10-pixel steps).

- Window Manager: Use your WM's resize method (e.g., Alt + Right Mouse Button in XFCE).

# Credits

Circam was conceptualized by b1tfl0w and developed with the extensive coding and debugging assistance of Grok, an AI created by xAI. Thank you, Grok, for writing the code and making this project a reality!


