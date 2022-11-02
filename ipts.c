#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <math.h>
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SCALE 16
#define WIDTH 64
#define HEIGHT 44
#define MAX_CLUSTER_SIZE 128
#define MAX_CLUSTERS 16

struct pixel {
  uint8_t x;
  uint8_t y;
  uint8_t value;
};

struct cluster {
  uint8_t size;
  struct pixel pixels[MAX_CLUSTER_SIZE];
  float centre_x;
  float centre_y;
  float x1;
  float y1;
  float x2;
  float y2;
  float diameter;
  uint8_t valid;
};

struct ipts_hid_header {
  uint8_t report;
  uint16_t timestamp;
  uint32_t size;
  uint8_t reserved1;
  uint8_t type;
  uint8_t reserved2;
} __attribute__((packed));

struct ipts_raw_header {
  uint32_t counter;
  uint32_t frames;
  uint8_t reserved[4];
} __attribute__((packed));

struct ipts_raw_frame_header {
  uint16_t index;
  uint16_t type;
  uint32_t size;
  uint8_t reserved[8];
} __attribute__((packed));

struct ipts_report_header {
  uint8_t type;
  uint8_t flags;
  uint16_t size;
} __attribute__((packed));

// Add a pixel to a cluster if it is dimmer than a threshold
// This function calls itself recursively to add surrounding pixels to the cluster until
// it enounters a pixel that is brighter than the previous one, or black.
void assign_group_dimmer(struct pixel *pixels, int x, int y, struct cluster *cluster, int threshold) {
  // Abort if the cluster has clready reached its maximum size
  if (cluster->size >= MAX_CLUSTER_SIZE) return;

  // Abort if the pixel is already in this cluster
  for (int n = 0; n < cluster->size; n++) {
    if (cluster->pixels[n].x == x && cluster->pixels[n].y == y) return;
  }
  // Abort if the pixel is black
  if (pixels[y * WIDTH + x].value == 0) return;
  // Abort if the pixel is brighter than the threshold
  if (pixels[y * WIDTH + x].value > threshold) return;

  // Add the pixel to the cluster
  cluster->pixels[cluster->size++] = pixels[y * WIDTH + x];

  // Call this function recursively for the surrounding pixels if they are within the image bounds
  if (x > 0) {
    if (y > 0) assign_group_dimmer(pixels, x - 1, y - 1, cluster, pixels[y * WIDTH + x].value);
    assign_group_dimmer(pixels, x - 1, y, cluster, pixels[y * WIDTH + x].value);
    if (y < HEIGHT - 1) assign_group_dimmer(pixels, x - 1, y + 1, cluster, pixels[y * WIDTH + x].value);
  }
  if (y > 0) assign_group_dimmer(pixels, x, y - 1, cluster, pixels[y * WIDTH + x].value);
  if (y < HEIGHT - 1) assign_group_dimmer(pixels, x, y + 1, cluster, pixels[y * WIDTH + x].value);
  if (x < WIDTH - 1) {
    if (y > 0) assign_group_dimmer(pixels, x + 1, y - 1, cluster, pixels[y * WIDTH + x].value);
    assign_group_dimmer(pixels, x + 1, y, cluster, pixels[y * WIDTH + x].value);
    if (y < HEIGHT - 1) assign_group_dimmer(pixels, x + 1, y + 1, cluster, pixels[y * WIDTH + x].value);
  }
}

// Identify whether a pixel is brighter than all its neighbours
int is_brightest(struct pixel *pixels, int x, int y) {
  if (pixels[y * WIDTH + x].value == 0) return 0;
  if (x > 0) {
    if (y > 0 && pixels[y * WIDTH + x].value < pixels[y * WIDTH - WIDTH + x - 1].value) return 0;
    if (pixels[y * WIDTH + x].value < pixels[y * WIDTH + x - 1].value) return 0;
    if (y < HEIGHT - 1 && pixels[y * WIDTH + x].value < pixels[y * WIDTH + WIDTH + x - 1].value) return 0;
  }
  if (y > 0 && pixels[y * WIDTH + x].value < pixels[y * WIDTH - WIDTH + x].value) return 0;
  if (y < HEIGHT - 1 && pixels[y * WIDTH + x].value < pixels[y * WIDTH + WIDTH + x].value) return 0;
  if (x < WIDTH - 1) {
    if (y > 0 && pixels[y * WIDTH + x].value < pixels[y * WIDTH - WIDTH + x + 1].value) return 0;
    if (pixels[y * WIDTH + x].value < pixels[y * WIDTH + x + 1].value) return 0;
    if (y < HEIGHT - 1 && pixels[y * WIDTH + x].value < pixels[y * WIDTH + WIDTH + x + 1].value) return 0;
  }

  return 1;
}

int main() {
  // Initialize SDL for testing
  SDL_Init(SDL_INIT_VIDEO);
  TTF_Init();
  SDL_Event event;
  SDL_Window *win = SDL_CreateWindow("Tablet", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH * SCALE, HEIGHT * SCALE, 0);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WIDTH * SCALE, HEIGHT * SCALE);
  TTF_Font *font = TTF_OpenFont("OpenSans-Regular.ttf", 24);

  // Open a hidraw device (or test file)
  int fd = open("hid.raw", O_RDWR);
  if (fd < 0) {
    perror("Error opening device/file");
    return 1;
  }

  // Call IOCTL to enable heatmaps on surface pro 5
  // uint8_t req[] = {66, 1};
  // int ret = ioctl(fd, HIDIOCSFEATURE(2), &req);
  // if (ret < 0) {
  //   perror("Error on ioctl HIDIOCSFEATURE");
  //   return 1;
  // }

  // Allocate memory for file reads, heatmap, and clusters
  void *buf = malloc(7485);
  struct pixel *pixels = malloc(sizeof(struct pixel) * WIDTH * HEIGHT);
  struct cluster *clusters = malloc(sizeof(struct cluster) * MAX_CLUSTERS);

  while (1) {
    // Exit on SDL quit event
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        return 0;
      }
    }
    // Read a frame from the device
    int n = read(fd, buf, 7485);
    if (n < 7485) {
      // Loop for testing, don't do this on a real device
      lseek(fd, 0, SEEK_SET);
      continue;
    }

    // Parse the received frame data
    int pos = 0;
    struct ipts_hid_header *ipts_hid_header = buf;
    pos += 10;
    // printf("Report: %i\n", ipts_hid_header->report);
    // printf("Size: %i\n", ipts_hid_header->size);
    // printf("Type: %i\n", ipts_hid_header->type);
    if (ipts_hid_header->type == 0xEE) {
      struct ipts_raw_header *ipts_raw_header = buf + pos;
      pos += 12;
      // printf("Counter: %i\n", ipts_raw_header->counter);
      // printf("Frames: %i\n", ipts_raw_header->frames);
      for (int n = 0; n < ipts_raw_header->frames; n++) {
        struct ipts_raw_frame_header *ipts_raw_frame_header = buf + pos;
        pos += 16;
        // printf("  Index: %i\n", ipts_raw_frame_header->index);
        // printf("  Type: %i\n", ipts_raw_frame_header->type);
        // printf("  Size: %i\n", ipts_raw_frame_header->size);
        int eof = pos + ipts_raw_frame_header->size;

        if (ipts_raw_frame_header->type == 6 || ipts_raw_frame_header->type == 8) {
          while (pos < eof) {
            struct ipts_report_header *ipts_report_header = buf + pos;
            pos += 4;
            // printf("    Report Type: %02x\n", ipts_report_header->type);
            // printf("    Report Size: %02x\n", ipts_report_header->size);
            if (ipts_report_header->type == 0x25) {
              // We have heatmap data, start processing!
              uint8_t *raw_pixels = buf + pos;

              // Copy pixels from raw frame and invert both axes as well as the values
              for (int y = 0; y < HEIGHT; y++) {
                for (int x = 0; x < WIDTH; x++) {
                  uint8_t xx = WIDTH - x - 1;
                  uint8_t yy = HEIGHT - y - 1;
                  int value = 255 - raw_pixels[yy * WIDTH + xx] - 90;
                  if (value < 0) value = 0;
                  pixels[y * WIDTH + x].value = value;
                  pixels[y * WIDTH + x].x = x;
                  pixels[y * WIDTH + x].y = y;
                }
              }

              // Group pixels into clusters
              memset(clusters, 0, sizeof(struct cluster) * MAX_CLUSTERS);
              int cluster_count = 0;
              for (int y = 0; y < HEIGHT; y++) {
                for (int x = 0; x < WIDTH; x++) {
                  // First identify the brightest pixels in the heatmap
                  // These are pixels that have no brighter neighbor
                  if (is_brightest(pixels, x, y)) {
                    // For each bright spot, create a cluster and add surrounding pixels to it recursively
                    if (cluster_count < MAX_CLUSTERS) assign_group_dimmer(pixels, x, y, &clusters[cluster_count++], pixels[y * WIDTH + x].value);
                  }
                }
              }

              // Calculate bounds of each cluster
              // We use floats to do this in the device space. We can convert to screen space later
              for (int i = 0; i < cluster_count; i++) {
                // Use each pixel's position and value to create a weighted average position
                float weighted_x = 0;
                float weighted_y = 0;
                float total_weight = 0;
                for (int j = 0; j < clusters[i].size; j++) {
                  weighted_x += clusters[i].pixels[j].x * clusters[i].pixels[j].value;
                  weighted_y += clusters[i].pixels[j].y * clusters[i].pixels[j].value;
                  total_weight += clusters[i].pixels[j].value;
                }
                clusters[i].centre_x = weighted_x / total_weight + 0.5;
                clusters[i].centre_y = weighted_y / total_weight + 0.5;
                clusters[i].diameter = total_weight / 100;
                // Use the centre of the cluster and total weight to approximate a bounding box
                clusters[i].x1 = clusters[i].centre_x - clusters[i].diameter / 2;
                clusters[i].y1 = clusters[i].centre_y - clusters[i].diameter / 2;
                clusters[i].x2 = clusters[i].centre_x + clusters[i].diameter / 2;
                clusters[i].y2 = clusters[i].centre_y + clusters[i].diameter / 2;
                // Mark all clusters as valid intially
                // We could add additional checks here to filter out clusters that are too small or too large
                clusters[i].valid = 1;
              }

              // Remove overlapping clusters
              for (int i = 0; i < cluster_count; i++) {
                for (int j = i + 1; j < cluster_count; j++) {
                  if (clusters[i].valid && clusters[j].valid) {
                    // Calculate the intersection of each pair of clusters
                    float intersection = fmax(0, fmin(clusters[i].x2, clusters[j].x2) - fmax(clusters[i].x1, clusters[j].x1)) * fmax(0, fmin(clusters[i].y2, clusters[j].y2) - fmax(clusters[i].y1, clusters[j].y1));
                    // Calculate the area of each cluster in the pair
                    float area_i = (clusters[i].x2 - clusters[i].x1) * (clusters[i].y2 - clusters[i].y1);
                    float area_j = (clusters[j].x2 - clusters[j].x1) * (clusters[j].y2 - clusters[j].y1);
                    // If the intersection is greater than 50% of the smaller cluster, invalidate it
                    if (area_i > area_j) {
                      if (intersection / area_j > 0.5) {
                        clusters[j].valid = 0;
                      }
                    } else {
                      if (intersection / area_i > 0.5) {
                        clusters[i].valid = 0;
                      }
                    }
                  }
                }
              }

              // Draw raw data to screen
              for (int y = 0; y < HEIGHT; y++) {
                for (int x = 0; x < WIDTH; x++) {
                  int xx = WIDTH - x - 1;
                  int yy = HEIGHT - y - 1;
                  uint8_t pixel = 255 - raw_pixels[yy * WIDTH + xx];
                  SDL_Rect rect;
                  rect.x = x * SCALE;
                  rect.y = y * SCALE;
                  rect.w = SCALE;
                  rect.h = SCALE;
                  SDL_SetRenderDrawColor(ren, pixel, pixel, pixel, 255);
                  SDL_RenderFillRect(ren, &rect);
                }
              }

              // Draw clusters to screen
              int valid_clusters = 0;
              for (int i = 0; i < cluster_count; i++) {
                SDL_Rect rect;
                rect.x = clusters[i].x1 * SCALE;
                rect.y = clusters[i].y1 * SCALE;
                rect.w = clusters[i].diameter * SCALE;
                rect.h = clusters[i].diameter * SCALE;
                if (clusters[i].valid) {
                  SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);
                  valid_clusters++;
                } else {
                  SDL_SetRenderDrawColor(ren, 255, 0, 0, 255);
                }
                SDL_RenderDrawRect(ren, &rect);
              }

              // Draw cluster count to screen
              char text[100];
              sprintf(text, "Clusters: %d", valid_clusters);
              SDL_Surface *surface;
              SDL_Color color = {0, 0, 0};
              surface = TTF_RenderText_Solid(font, text, color);
              SDL_Texture *texture = SDL_CreateTextureFromSurface(ren, surface);
              SDL_Rect dstrect = {0, 0, surface->w, surface->h};
              SDL_FreeSurface(surface);
              SDL_RenderCopy(ren, texture, NULL, &dstrect);
              SDL_DestroyTexture(texture);

              // Update screen
              SDL_RenderPresent(ren);

              // Sleep 100ms
              // nanosleep((const struct timespec[]){{0, 100000000L}}, NULL);
            }
            pos += ipts_report_header->size;
          }
        } else {
          pos = eof;
        }
      }
    }
    // printf("\n");
    fflush(stdout);
  }
  return 0;
}
