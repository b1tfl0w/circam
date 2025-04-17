#include <SDL2/SDL.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define DEFAULT_SIZE 480
#define MIN_SIZE 100
#define MIN_WINDOW_SIZE 50
#define SIZE_STEP 10 // Resize step for keyboard and mouse wheel
#define RESIZE_STABILIZE_MS 100 // Wait for mouse resize to stabilize

// Structure to hold buffer information
struct buffer {
    void *start;
    size_t length;
};

// Global variables for dragging and resizing
static int dragging = 0;
static int drag_start_x, drag_start_y; // Screen coordinates at drag start
static int win_start_x, win_start_y;   // Window position at drag start
static int pending_resize = 0;         // Flag for pending resize
static int pending_size = 0;           // Requested size for pending resize
static Uint32 last_resize_time = 0;    // Time of last resize event

// Function to create or update the circular shape
SDL_Surface *create_circular_shape(int size) {
    SDL_Surface *surface = SDL_CreateRGBSurface(0, size, size, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000);
    if (!surface) {
        fprintf(stderr, "SDL_CreateRGBSurface failed: %s\n", SDL_GetError());
        return NULL;
    }
    SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 0, 0, 0, 0));
    int center = size / 2;
    int radius = center * center;
    Uint32 white = SDL_MapRGBA(surface->format, 255, 255, 255, 255);
    Uint32 *pixels = (Uint32 *)surface->pixels;
    for (int y = 0; y < size; y++) {
        int dy = y - center;
        int dy2 = dy * dy;
        for (int x = 0; x < size; x++) {
            int dx = x - center;
            if (dx * dx + dy2 <= radius) {
                pixels[y * size + x] = white;
            }
        }
    }
    return surface;
}

int main(int argc, char *argv[]) {
    int window_size = DEFAULT_SIZE;
    char *video_device = NULL;
    int always_on_top = 0;

    // Parse command-line arguments
    if (argc < 2 || argc > 5) {
        fprintf(stderr, "Usage: %s [-t] [-s <size>] <video_device>\n", argv[0]);
        fprintf(stderr, "Example: %s -t -s 256 /dev/video0\n", argv[0]);
        return 1;
    }

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-t") == 0) {
            always_on_top = 1;
            i++;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -s requires a size value\n");
                return 1;
            }
            window_size = atoi(argv[i + 1]);
            if (window_size < MIN_SIZE) {
                fprintf(stderr, "Error: Size must be at least %d pixels\n", MIN_SIZE);
                return 1;
            }
            i += 2;
        } else {
            video_device = argv[i];
            i++;
        }
    }

    if (!video_device) {
        fprintf(stderr, "Error: No video device specified\n");
        fprintf(stderr, "Usage: %s [-t] [-s <size>] <video_device>\n", argv[0]);
        return 1;
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Open V4L2 device
    int fd = open(video_device, O_RDWR, 0);
    if (fd < 0) {
        perror("Cannot open device");
        SDL_Quit();
        return 1;
    }

    // Query device capabilities
    struct v4l2_capability cap;
    CLEAR(cap);
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        close(fd);
        SDL_Quit();
        return 1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "Device does not support video capture\n");
        close(fd);
        SDL_Quit();
        return 1;
    }

    // Set format (YUYV, 640x480)
    struct v4l2_format fmt;
    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        close(fd);
        SDL_Quit();
        return 1;
    }

    // Calculate crop rectangle for square
    int crop_size = fmt.fmt.pix.width < fmt.fmt.pix.height ? fmt.fmt.pix.width : fmt.fmt.pix.height;
    SDL_Rect src_rect = {
        .x = (fmt.fmt.pix.width - crop_size) / 2,
        .y = (fmt.fmt.pix.height - crop_size) / 2,
        .w = crop_size,
        .h = crop_size
    };

    // Request buffers
    struct v4l2_requestbuffers req;
    CLEAR(req);
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        close(fd);
        SDL_Quit();
        return 1;
    }

    // Map buffers
    struct buffer *buffers = calloc(req.count, sizeof(*buffers));
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            free(buffers);
            close(fd);
            SDL_Quit();
            return 1;
        }
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            perror("mmap");
            free(buffers);
            close(fd);
            SDL_Quit();
            return 1;
        }
    }

    // Queue buffers
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            free(buffers);
            close(fd);
            SDL_Quit();
            return 1;
        }
    }

    // Start streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        free(buffers);
        close(fd);
        SDL_Quit();
        return 1;
    }

    // Create a resizable shaped window with optional always-on-top
    Uint32 window_flags = SDL_WINDOW_RESIZABLE;
    if (always_on_top) {
        window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    SDL_Window *window = SDL_CreateShapedWindow("Circam", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_size, window_size, window_flags);
    if (!window) {
        fprintf(stderr, "SDL_CreateShapedWindow failed: %s\n", SDL_GetError());
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        free(buffers);
        close(fd);
        SDL_Quit();
        return 1;
    }

    // Explicitly enable resizing
    SDL_SetWindowResizable(window, SDL_TRUE);

    // Create renderer
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        free(buffers);
        close(fd);
        SDL_Quit();
        return 1;
    }

    // Create initial circular shape
    SDL_Surface *shape_surface = create_circular_shape(window_size);
    if (!shape_surface) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        free(buffers);
        close(fd);
        SDL_Quit();
        return 1;
    }
    SDL_WindowShapeMode mode = { .mode = ShapeModeBinarizeAlpha, .parameters.binarizationCutoff = 255 };
    SDL_SetWindowShape(window, shape_surface, &mode);

    // Create texture for YUYV
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YUY2, SDL_TEXTUREACCESS_STREAMING, fmt.fmt.pix.width, fmt.fmt.pix.height);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_FreeSurface(shape_surface);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        free(buffers);
        close(fd);
        SDL_Quit();
        return 1;
    }

    // Track current window size
    int current_window_size = window_size;

    // Main loop
    SDL_Event event;
    int running = 1;
    while (running) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = 0;
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        running = 0;
                    } else if (event.key.keysym.sym == SDLK_PLUS || event.key.keysym.sym == SDLK_EQUALS) {
                        // Increase size
                        window_size = current_window_size + SIZE_STEP;
                        if (window_size < MIN_WINDOW_SIZE) window_size = MIN_WINDOW_SIZE;
                        SDL_SetWindowSize(window, window_size, window_size);
                        SDL_FreeSurface(shape_surface);
                        shape_surface = create_circular_shape(window_size);
                        if (shape_surface) {
                            SDL_SetWindowShape(window, shape_surface, &mode);
                        }
                        current_window_size = window_size;
                        // printf("Keyboard resized to %dx%d\n", window_size, window_size);
                    } else if (event.key.keysym.sym == SDLK_MINUS) {
                        // Decrease size
                        window_size = current_window_size - SIZE_STEP;
                        if (window_size < MIN_WINDOW_SIZE) window_size = MIN_WINDOW_SIZE;
                        SDL_SetWindowSize(window, window_size, window_size);
                        SDL_FreeSurface(shape_surface);
                        shape_surface = create_circular_shape(window_size);
                        if (shape_surface) {
                            SDL_SetWindowShape(window, shape_surface, &mode);
                        }
                        current_window_size = window_size;
                        // printf("Keyboard resized to %dx%d\n", window_size, window_size);
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        dragging = 1;
                        SDL_GetGlobalMouseState(&drag_start_x, &drag_start_y);
                        SDL_GetWindowPosition(window, &win_start_x, &win_start_y);
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        dragging = 0;
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (dragging) {
                        int mouse_x, mouse_y;
                        SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
                        int new_x = win_start_x + (mouse_x - drag_start_x);
                        int new_y = win_start_y + (mouse_y - drag_start_y);
                        SDL_SetWindowPosition(window, new_x, new_y);
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    if (event.wheel.y > 0) { // Wheel up
                        window_size = current_window_size + SIZE_STEP;
                    } else if (event.wheel.y < 0) { // Wheel down
                        window_size = current_window_size - SIZE_STEP;
                    }
                    if (window_size < MIN_WINDOW_SIZE) window_size = MIN_WINDOW_SIZE;
                    SDL_SetWindowSize(window, window_size, window_size);
                    SDL_FreeSurface(shape_surface);
                    shape_surface = create_circular_shape(window_size);
                    if (shape_surface) {
                        SDL_SetWindowShape(window, shape_surface, &mode);
                    }
                    current_window_size = window_size;
                    // printf("Mouse wheel resized to %dx%d\n", window_size, window_size);
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        int new_size = event.window.data1 < event.window.data2 ? event.window.data1 : event.window.data2;
                        if (new_size >= MIN_WINDOW_SIZE && new_size != current_window_size) {
                            pending_resize = 1;
                            pending_size = new_size;
                            last_resize_time = SDL_GetTicks();
                            // printf("Resize requested to %dx%d\n", new_size, new_size);
                        }
                    }
                    break;
            }
        }

        // Check for stabilized resize
        if (pending_resize && (SDL_GetTicks() - last_resize_time >= RESIZE_STABILIZE_MS)) {
            SDL_SetWindowSize(window, pending_size, pending_size);
            int w, h;
            SDL_GetWindowSize(window, &w, &h);
            if (w == h && w == pending_size) {
                window_size = pending_size;
                current_window_size = window_size;
                SDL_FreeSurface(shape_surface);
                shape_surface = create_circular_shape(window_size);
                if (shape_surface) {
                    SDL_SetWindowShape(window, shape_surface, &mode);
                }
                // printf("Window resized to %dx%d (actual %dx%d)\n", window_size, window_size, w, h);
            } else {
                printf("Resize failed: requested %dx%d, actual %dx%d\n", pending_size, pending_size, w, h);
            }
            pending_resize = 0;
        }

        // Render cropped square using current window size
        SDL_Rect dst_rect = { .x = 0, .y = 0, .w = current_window_size, .h = current_window_size };
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, &src_rect, &dst_rect);
        SDL_RenderPresent(renderer);

        // Debug: Log sizes
        //int w, h;
        //SDL_GetWindowSize(window, &w, &h);
        //printf("Render: window_size=%d, actual_size=%dx%d, dst_rect(w=%d, h=%d, x=%d, y=%d)\n",
        //       current_window_size, w, h, dst_rect.w, dst_rect.h, dst_rect.x, dst_rect.y);

        // Wait for buffer using select
        fd_set fds;
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r < 0) {
            perror("select");
            continue;
        }
        if (r == 0) {
            fprintf(stderr, "select timeout\n");
            continue;
        }

        // Dequeue buffer
        struct v4l2_buffer buf;
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("VIDIOC_DQBUF");
            continue;
        }

        // Update texture with new frame
        SDL_UpdateTexture(texture, NULL, buffers[buf.index].start, fmt.fmt.pix.width * 2);

        // Requeue buffer
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
        }
    }

    // Cleanup
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(shape_surface);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (unsigned int i = 0; i < req.count; i++) {
        munmap(buffers[i].start, buffers[i].length);
    }
    free(buffers);
    close(fd);
    SDL_Quit();

    return 0;
}