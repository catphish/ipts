#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/uinput.h>
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
  int id;
};

struct cluster_group {
  uint8_t size;
  struct cluster clusters[MAX_CLUSTERS];
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

struct ipts_stylus_report {
  uint8_t elements;
  uint8_t reserved[3];
  uint32_t serial;
} __attribute__((packed));

struct ipts_stylus_element {
  uint16_t timestamp;
  uint16_t mode;
  uint16_t x;
  uint16_t y;
  uint16_t pressure;
  uint16_t altitude;
  uint16_t azimuth;
  uint8_t reserved[2];
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

void emit(int fd, int type, int code, int val) {
  struct input_event ie;

  ie.type = type;
  ie.code = code;
  ie.value = val;
  /* timestamp values below are ignored */
  ie.time.tv_sec = 0;
  ie.time.tv_usec = 0;

  write(fd, &ie, sizeof(ie));
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

  // Open uinput device
  int uinput = -1;
  // uinput = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  ioctl(uinput, UI_SET_EVBIT, EV_KEY);
  ioctl(uinput, UI_SET_KEYBIT, BTN_TOUCH);

  ioctl(uinput, UI_SET_EVBIT, EV_ABS);
  ioctl(uinput, UI_SET_ABSBIT, ABS_X);
  ioctl(uinput, UI_SET_ABSBIT, ABS_Y);

  ioctl(uinput, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

  ioctl(uinput, UI_SET_ABSBIT, ABS_MT_SLOT);
  ioctl(uinput, UI_SET_ABSBIT, ABS_MT_POSITION_X);
  ioctl(uinput, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
  ioctl(uinput, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
  ioctl(uinput, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);

  struct uinput_setup usetup;
  memset(&usetup, 0, sizeof(usetup));
  usetup.id.bustype = BUS_USB;
  usetup.id.vendor = 0x1234;  /* sample vendor */
  usetup.id.product = 0x5678; /* sample product */
  strcpy(usetup.name, "Test tablet device");
  ioctl(uinput, UI_DEV_SETUP, &usetup);

  struct uinput_abs_setup abs;
  memset(&abs, 0, sizeof(abs));
  abs.code = ABS_X;
  abs.absinfo.maximum = WIDTH * SCALE;
  ioctl(uinput, UI_ABS_SETUP, &abs);
  abs.code = ABS_MT_POSITION_X;
  ioctl(uinput, UI_ABS_SETUP, &abs);

  abs.code = ABS_Y;
  abs.absinfo.maximum = HEIGHT * SCALE;
  ioctl(uinput, UI_ABS_SETUP, &abs);
  abs.code = ABS_MT_POSITION_Y;
  ioctl(uinput, UI_ABS_SETUP, &abs);

  abs.code = ABS_MT_SLOT;
  abs.absinfo.maximum = 10;
  ioctl(uinput, UI_ABS_SETUP, &abs);
  abs.code = ABS_MT_TRACKING_ID;
  abs.absinfo.maximum = 10;
  ioctl(uinput, UI_ABS_SETUP, &abs);
  abs.code = ABS_MT_TOUCH_MAJOR;
  abs.absinfo.maximum = 1000;
  ioctl(uinput, UI_ABS_SETUP, &abs);

  ioctl(uinput, UI_DEV_CREATE);

  // Open a hidraw device (or test file)
  // int fd = open("/dev/hidraw0", O_RDWR);
  // if (fd < 0) {
  //   fd = open("/dev/hidraw1", O_RDWR);
  //   if (fd < 0) {
  //     perror("Error opening device/file");
  //     return 1;
  //   }
  // }
  int fd = open("hid2.raw", O_RDWR);
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
  struct cluster_group *cluster_groups = malloc(sizeof(struct cluster_group) * 2);
  memset(cluster_groups, 0, sizeof(struct cluster_group) * 2);
  int current_cluster_group = 0;

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

    struct cluster_group *cluster_group = &cluster_groups[current_cluster_group];
    memset(cluster_group, 0, sizeof(struct cluster_group));
    struct cluster_group *previous_cluster_group = &cluster_groups[current_cluster_group ^ 1];
    struct cluster *clusters = cluster_group->clusters;
    struct cluster *previous_clusters = previous_cluster_group->clusters;
    current_cluster_group ^= 1;

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
            // printf("    Report Type: %08x\n", ipts_report_header->type);
            // printf("    Report Size: %02x\n", ipts_report_header->size);
            if (ipts_report_header->type == 0x60) {
              struct ipts_stylus_report *ipts_stylus_report = buf + pos;
              printf("Stylus data! Serial: %d\n", ipts_stylus_report->serial);
              // Loop through stylus elements
              for (int n = 0; n < ipts_stylus_report->elements; n++) {
                struct ipts_stylus_element *ipts_stylus_element = buf + pos + 8 + n * 16;
                printf("  Element: Mode: %02x, X: %d, Y: %d\n", ipts_stylus_element->mode, ipts_stylus_element->x, ipts_stylus_element->y);
                printf("    Pressure: %d, Altitude: %d, Azimuth: %d\n", ipts_stylus_element->pressure, ipts_stylus_element->altitude, ipts_stylus_element->azimuth);
              }
            } else if (ipts_report_header->type == 0x25) {
              // We have heatmap data, start processing!
              uint8_t *raw_pixels = buf + pos;
              int disable_touch = 0;

              // Copy pixels from raw frame and invert both axes as well as the values
              for (int y = 0; y < HEIGHT; y++) {
                for (int x = 0; x < WIDTH; x++) {
                  uint8_t xx = WIDTH - x - 1;
                  uint8_t yy = HEIGHT - y - 1;
                  int value = 255 - raw_pixels[yy * WIDTH + xx] - 100;
                  if (value < 0) value = 0;
                  pixels[y * WIDTH + x].value = value;
                  pixels[y * WIDTH + x].x = x;
                  pixels[y * WIDTH + x].y = y;
                }
              }

              // Group pixels into clusters
              cluster_group->size = 0;
              for (int y = 0; y < HEIGHT; y++) {
                for (int x = 0; x < WIDTH; x++) {
                  // First identify the brightest pixels in the heatmap
                  // These are pixels that have no brighter neighbor
                  if (is_brightest(pixels, x, y)) {
                    // For each bright spot, create a cluster and add surrounding pixels to it recursively
                    if (cluster_group->size < MAX_CLUSTERS) assign_group_dimmer(pixels, x, y, &clusters[cluster_group->size++], pixels[y * WIDTH + x].value);
                  }
                }
              }

              // Calculate bounds of each cluster
              // We use floats to do this in the device space. We can convert to screen space later
              for (int i = 0; i < cluster_group->size; i++) {
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
                if (clusters[i].diameter > 0.5f) clusters[i].valid = 1;
                if (clusters[i].diameter > 10.f) disable_touch = 1;
              }

              if (disable_touch) {
                // If we have a cluster that is too large, disable touch
                for (int i = 0; i < cluster_group->size; i++) {
                  clusters[i].valid = 0;
                }
              }

              // Remove overlapping clusters
              for (int i = 0; i < cluster_group->size; i++) {
                for (int j = i + 1; j < cluster_group->size; j++) {
                  if (clusters[i].valid && clusters[j].valid) {
                    // Calculate the intersection of each pair of clusters
                    float intersection = fmax(0, fmin(clusters[i].x2, clusters[j].x2) - fmax(clusters[i].x1, clusters[j].x1)) * fmax(0, fmin(clusters[i].y2, clusters[j].y2) - fmax(clusters[i].y1, clusters[j].y1));
                    // Calculate the area of each cluster in the pair
                    float area_i = (clusters[i].x2 - clusters[i].x1) * (clusters[i].y2 - clusters[i].y1);
                    float area_j = (clusters[j].x2 - clusters[j].x1) * (clusters[j].y2 - clusters[j].y1);
                    // If the intersection is greater than 50% of the smaller cluster, invalidate it
                    if (area_i > area_j) {
                      if (intersection / area_j > 0.25) {
                        clusters[j].valid = 0;
                      }
                    } else {
                      if (intersection / area_i > 0.25) {
                        clusters[i].valid = 0;
                      }
                    }
                  }
                }
              }

              // Attempt to collelate clusters with those from previous frames
              // This is done by iterating through previous clusters and finding the closest match in the current frame
              for (int n = 0; n < previous_cluster_group->size; n++) {
                if (!previous_clusters[n].valid) continue;
                float closest_distance = 1000000;
                int closest_index = -1;
                for (int m = 0; m < cluster_group->size; m++) {
                  if (clusters[m].valid && clusters[m].id == 0) {
                    float distance = pow(clusters[m].centre_x - previous_clusters[n].centre_x, 2) + pow(clusters[m].centre_y - previous_clusters[n].centre_y, 2);
                    if (distance < closest_distance) {
                      closest_distance = distance;
                      closest_index = m;
                    }
                  }
                }
                if (closest_index != -1) {
                  clusters[closest_index].id = previous_clusters[n].id;
                }
              }

              // Assign new IDs to any clusters that don't have one yet
              for (int m = 0; m < cluster_group->size; m++) {
                if (clusters[m].valid && clusters[m].id == 0) {
                  // Find the lowest unused ID
                  int id = 1;
                  while (1) {
                    int found = 0;
                    for (int n = 0; n < cluster_group->size; n++) {
                      if (clusters[n].id == id) {
                        found = 1;
                        break;
                      }
                    }
                    if (!found) break;
                    id++;
                  }
                  clusters[m].id = id;
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
              for (int i = 0; i < cluster_group->size; i++) {
                SDL_Rect rect;
                rect.x = clusters[i].x1 * SCALE;
                rect.y = clusters[i].y1 * SCALE;
                rect.w = clusters[i].diameter * SCALE;
                rect.h = clusters[i].diameter * SCALE;
                if (clusters[i].valid) {
                  SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);
                  valid_clusters++;
                  char text[100];
                  sprintf(text, "%d", clusters[i].id);
                  SDL_Surface *surface;
                  SDL_Color color = {0, 0, 0};
                  surface = TTF_RenderText_Solid(font, text, color);
                  SDL_Texture *texture = SDL_CreateTextureFromSurface(ren, surface);
                  SDL_Rect dstrect = {rect.x, rect.y, surface->w, surface->h};
                  SDL_FreeSurface(surface);
                  SDL_RenderCopy(ren, texture, NULL, &dstrect);
                  SDL_DestroyTexture(texture);
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

              valid_clusters = 0;
              for (int n = 0; n < cluster_group->size; n++) {
                if (clusters[n].valid) {
                  valid_clusters++;
                }
              }

              // Emit to uinput
              for (int n = 0; n < 6; n++) {
                emit(uinput, EV_ABS, ABS_MT_SLOT, n);
                int tracking_id = -1;
                for (int i = 0; i < cluster_group->size; i++) {
                  if (clusters[i].id == n + 1 && clusters[i].valid) {
                    emit(uinput, EV_ABS, ABS_MT_POSITION_X, clusters[i].centre_x * SCALE);
                    emit(uinput, EV_ABS, ABS_MT_POSITION_Y, clusters[i].centre_y * SCALE);
                    emit(uinput, EV_ABS, ABS_MT_TOUCH_MAJOR, clusters[i].diameter * SCALE);
                    tracking_id = clusters[i].id;
                    if (valid_clusters == 1) {
                      emit(uinput, EV_ABS, ABS_X, clusters[i].centre_x * SCALE);
                      emit(uinput, EV_ABS, ABS_Y, clusters[i].centre_y * SCALE);
                      emit(uinput, EV_KEY, BTN_TOUCH, 1);
                    }
                  }
                }
                emit(uinput, EV_ABS, ABS_MT_TRACKING_ID, tracking_id);
              }
              if (valid_clusters != 1) {
                emit(uinput, EV_KEY, BTN_TOUCH, 0);
              }

              emit(uinput, EV_SYN, SYN_REPORT, 0);

              // Sleep 100ms
              // nanosleep((const struct timespec[]){{0, 50000000L}}, NULL);
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
