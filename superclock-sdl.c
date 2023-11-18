// Included header files.
#define _GNU_SOURCE

//https://fonts.google.com/icons?selected=Material+Symbols+Outlined:power_off:FILL@0;wght@300;GRAD@0;opsz@40&icon.platform=web

#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <pthread.h>
#include <errno.h>
#include <json-c/json.h>
#include "mq.h"
#include "dlog.h"
#include "dmem.h"
#include "dfork.h"

// Define directives for constants.
#define MY_SDL_FLAGS (SDL_INIT_VIDEO|SDL_INIT_TIMER/*|SDL_INIT_AUDIO*/)
#define TITLE "Super Clock - SDL"
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

SDL_Color rgba_green = {0, 255, 0, 255};
SDL_Color rgba_red = {255, 0, 0, 255};
SDL_Color rgba_yellow = {255, 255, 0, 255};
SDL_Color rgba_background = {24, 90, 147, 255};
SDL_Color rgba_black = {0, 0, 0, 255};
SDL_Color rgba_white = {255, 255, 255, 255};
SDL_Color rgba_grey = {112, 112, 112, 255};

struct main_power_t {
    double power;
    double voltage;
    bool online;
    bool changed;
} main_power = {NAN, NAN, false, true};

struct battery_t {
    double soc;
    double current;
    double voltage;
    double temp;
    double capacity;
    bool online;
    bool changed;
} battery = {NAN, NAN, NAN, NAN, NAN, false, true};

struct superclock {
    SDL_Window *win;
    SDL_Renderer *rend;
    int window_width;
    int window_height;
    bool running;
    bool show_time;
    unsigned short exit_status;
};

typedef enum {
    ALIGN_LEFT,
    ALIGN_H_CENTER,
    ALIGN_RIGHT
} align_h_t;

typedef enum {
    ALIGN_TOP,
    ALIGN_V_CENTER,
    ALIGN_DOWN
} align_v_t;

typedef struct {
    align_h_t align_h;
    align_v_t align_v;
} align_t;


struct ITEM_T;

typedef void (*on_click_cb_t)(struct ITEM_T *item);

typedef struct ITEM_T {
    const char *name;
    SDL_Point position;
    SDL_Texture *texture;
    void *custom_data;

    SDL_Texture *(*update)(SDL_Renderer *renderer, struct ITEM_T *);

    bool texture_changed;
    align_t align;
    on_click_cb_t on_click;
    struct ITEM_T *next;
} item_t;

// forward declaration of functions.

unsigned short sdl_setup(struct superclock *sc);

Uint32 timer_show_time();

void memory_release_exit(struct superclock *sc);

int brightnessSet(int value);

int brightnessGet(void);

void item_add(item_t **head, item_t *item) {
    item->next = *head;
    *head = item;
}

typedef struct {
    SDL_Color color;
    char *text;
} color_text_item_t;

typedef struct {
    TTF_Font *font;
    time_t last_time;
    color_text_item_t text;
} time_item_t;

SDL_Texture *printf_SDL_Texture(SDL_Renderer *renderer, time_item_t *item, SDL_Color color, const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *buf = NULL;
    vasprintf(&buf, format, args);
    va_end(args);
    if (buf) {
        daemon_log(LOG_INFO, "buf: %s", buf);
        bool changed = false;
        if (memcmp(&color, &item->text.color, sizeof(SDL_Color))) {
            item->text.color = color;
            daemon_log(LOG_INFO, "color changed");
            changed = true;
        }
        if (item->text.text) {
            if (strcmp(item->text.text, buf)) {
                daemon_log(LOG_INFO, "text changed %s %s", item->text.text, buf);
                FREE(item->text.text);
                item->text.text = buf;
                changed = true;
            } else {
                FREE(buf);
            }
        } else {
            daemon_log(LOG_INFO, "text changed %s", buf);
            item->text.text = buf;
            changed = true;
        }
        if (changed) {
            SDL_Surface *surface = TTF_RenderText_Solid(item->font, item->text.text, item->text.color);
            SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_FreeSurface(surface);
            return texture;
        }
    }
    return NULL;
}

void item_free(item_t *head) {
    while (head) {
        item_t *next = head->next;
        SDL_DestroyTexture(head->texture);
        FREE(head);
        head = next;
    }
}

item_t *
item_new(const char *name, SDL_Renderer *renderer, SDL_Point position, align_t align, void *custom_data,
         SDL_Texture *(*update)(SDL_Renderer *renderer, struct ITEM_T *)) {
    item_t *item = calloc(1, sizeof(item_t));
    if (item) {
        item->name = name;
        item->position = position;
        item->align = align;
        item->custom_data = custom_data;
        item->update = update;
        item->texture = update(renderer, item);
        item->texture_changed = true;
    }
    return item;
}

SDL_Color lerp_color(const SDL_Color color1, const SDL_Color color2, float t) {
    SDL_Color result;
    result.r = (Uint8)(color1.r + t * (color2.r - color1.r));
    result.g = (Uint8)(color1.g + t * (color2.g - color1.g));
    result.b = (Uint8)(color1.b + t * (color2.b - color1.b));
    result.a = (Uint8)(color1.a + t * (color2.a - color1.a));
    return result;
}

/*********************************************************************************************************************/
struct weather_t {
    double temperature_indoor;
    double temperature_outdoor;
    bool temperature_outdoor_online;
    bool temperature_indoor_online;
    bool temperature_indoor_changed;
    bool temperature_outdoor_changed;
} weather = {NAN, NAN, false, false, true, true};

struct door_t {
    bool online;
    bool open;
    bool changed;
} door = {false, false, true};

void *indoor_temp_create(void) {
    time_item_t *item = calloc(1, sizeof(time_item_t));
    if (item) {
        item->font = TTF_OpenFont("/home/palich/bin/freesansbold.ttf", 50);
        if (!item->font) {
            printf("TTF_OpenFont: %s\n", TTF_GetError());
            FREE(item);
        }
    }
    return item;
}

SDL_Texture *indoor_temp_update(SDL_Renderer *renderer, struct ITEM_T *_item) {
    time_item_t *item = _item->custom_data;
    if (!item || !item->font) {
        return NULL;
    }
    if (weather.temperature_indoor_changed) {
        weather.temperature_indoor_changed = false;
        if (weather.temperature_indoor_online && !isnan(weather.temperature_indoor)) {
            return printf_SDL_Texture(renderer, item, rgba_white, "%.1fC",
                                      weather.temperature_indoor);
        } else {
            return printf_SDL_Texture(renderer, item, rgba_grey, "--.-C");
        }
    }
    return NULL;
}

void *outdoor_temp_create(void) {
    time_item_t *item = calloc(1, sizeof(time_item_t));
    if (item) {
        item->font = TTF_OpenFont("/home/palich/bin/freesansbold.ttf", 50);
        if (!item->font) {
            printf("TTF_OpenFont: %s\n", TTF_GetError());
            FREE(item);
        }
    }
    return item;
}

SDL_Texture *outdoor_temp_update(SDL_Renderer *renderer, struct ITEM_T *_item) {
    time_item_t *item = _item->custom_data;
    if (!item || !item->font) {
        return NULL;
    }

    if (weather.temperature_outdoor_changed) {
        weather.temperature_outdoor_changed = false;
        if (weather.temperature_outdoor_online && !isnan(weather.temperature_outdoor)) {
            return printf_SDL_Texture(renderer, item, rgba_white, "%.1fC",
                                      weather.temperature_outdoor);
        } else {
            return printf_SDL_Texture(renderer, item, rgba_grey, "--.-C");
        }
    }
    return NULL;
}

void *power_create() {
    time_item_t *item = calloc(1, sizeof(time_item_t));
    if (item) {
        item->font = TTF_OpenFont("/home/palich/bin/freesansbold.ttf", 25);
        if (!item->font) {
            printf("TTF_OpenFont: %s\n", TTF_GetError());
            FREE(item);
        }
    }
    return item;
}

SDL_Texture *power_update(SDL_Renderer *renderer, struct ITEM_T *_item) {
    time_item_t *item = _item->custom_data;
    if (!item || !item->font) {
        return NULL;
    }
    if (main_power.changed) {
        main_power.changed = false;
        if (main_power.online && !isnan(main_power.power)) {
            SDL_Color color = rgba_green;
            if (main_power.power > 1000.0) {
                color = rgba_yellow;
            } else if (main_power.power > 4000.0) {
                color = rgba_red;
            }
            return printf_SDL_Texture(renderer, item, color, "%.0fW %.0fV",
                                      main_power.power, main_power.voltage);
        } else {
            return printf_SDL_Texture(renderer, item, rgba_grey, "%.0fW %.0fV",
                                      0.0, 0);
        }
    }
    return NULL;
}


void *battery_create() {
    time_item_t *item = calloc(1, sizeof(time_item_t));
    if (item) {
        item->font = TTF_OpenFont("/home/palich/bin/freesansbold.ttf", 25);
        if (!item->font) {
            printf("TTF_OpenFont: %s\n", TTF_GetError());
            FREE(item);
        }
    }
    return item;
}

SDL_Texture *battery_update(SDL_Renderer *renderer, struct ITEM_T *_item) {
    time_item_t *item = _item->custom_data;
    if (!item || !item->font) {
        return NULL;
    }
    if (battery.changed) {
        battery.changed = false;
        if (battery.online && !isnan(battery.soc)) {
            SDL_Color color = rgba_green;
            if (battery.soc < 20.0) {
                color = rgba_red;
            } else if (battery.soc < 50.0) {
                color = rgba_yellow;
            } else if (battery.temp > 50.0) {
                color = rgba_red;
            } else if (battery.temp > 40.0) {
                color = rgba_yellow;
            }
            daemon_log(LOG_INFO, "battery: %.0f%% %.2fA %.0fC",
                       battery.soc, battery.current, battery.temp
            );
            if (battery.current > 0.2) {
                return printf_SDL_Texture(renderer, item, color, "%.0f%% %.0fW %.0fC %.0fh",
                                          battery.soc, battery.current * battery.voltage, battery.temp,
                                          (280 - battery.capacity) / battery.current);
            } else if (battery.current < -0.2) {
                return printf_SDL_Texture(renderer, item, color, "%.0f%% %.0fW %.0fC %.0fh",
                                          battery.soc, battery.current * battery.voltage, battery.temp,
                                          battery.capacity / (-battery.current));
            }
            return printf_SDL_Texture(renderer, item, color, "%.0f%% %.0fC",
                                      battery.soc, battery.temp);
        } else {
            return printf_SDL_Texture(renderer, item, rgba_grey, "%.0f%% %.0fW %.0fC",
                                      0.0, 0.0, 0.0);
        }
    }
    return NULL;
}

void *time_create() {
    time_item_t *item = calloc(1, sizeof(time_item_t));
    if (item) {
        item->font = TTF_OpenFont("/home/palich/bin/freesansbold.ttf", 55);
        if (!item->font) {
            printf("TTF_OpenFont: %s\n", TTF_GetError());
            FREE(item);
        }
    }
    return item;
}

SDL_Texture *time_update(SDL_Renderer *renderer, struct ITEM_T *_item) {
    time_item_t *item = _item->custom_data;
    if (!item || !item->font) {
        return NULL;
    }
    time_t now;
    time(&now);
    time_t minutes = now / 60;
    if (item->last_time != minutes) {
        item->last_time = minutes;
        struct tm *now_local = localtime(&now);
        SDL_Color color = {255, 255, 255, 255};
        return printf_SDL_Texture(renderer, item, color, "%02d:%02d", now_local->tm_hour,
                                  now_local->tm_min);
    }
    return NULL;
}

/*********************************************************************************************************************/
typedef struct {
    SDL_Surface *surface;
    time_t last_time;
    bool on;
} img_item_t;

void *img_create(const char *file) {
    img_item_t *item = calloc(1, sizeof(img_item_t));
    if (item) {
        item->surface = IMG_Load(file);
        if (!item->surface) {
            printf("IMG_Load: %s\n", IMG_GetError());
            FREE(item);
        }
    }
    return item;
}

void img_destroy(img_item_t *item) {
    if (item) {
        SDL_FreeSurface(item->surface);
        FREE(item);
    }
}

SDL_Texture *colorizeTexture(SDL_Renderer *renderer, const SDL_Surface *surface, SDL_Color c);

SDL_Texture *img_main_power_update(SDL_Renderer *renderer, struct ITEM_T *_item) {
    img_item_t *item = _item->custom_data;
    if (!item || !item->surface) {
        return NULL;
    }

    static int last_online = -1;
    if (main_power.online != last_online) {
        last_online = main_power.online;
        if (main_power.online) {
            return colorizeTexture(renderer, item->surface, rgba_green);
        } else {
            return colorizeTexture(renderer, item->surface, rgba_background);
        }
    }
    return NULL;
}

SDL_Texture *img_main_power_update2(SDL_Renderer *renderer, struct ITEM_T *_item) {
    img_item_t *item = _item->custom_data;
    if (!item || !item->surface) {
        return NULL;
    }

    static int last_online = -1;
    if (main_power.online != last_online) {
        last_online = main_power.online;
        if (!main_power.online) {
            return colorizeTexture(renderer, item->surface, rgba_red);
        } else {
            return colorizeTexture(renderer, item->surface, rgba_background);
        }
    }
    return NULL;
}

static bool power_off_pressed = false;

void on_click_power_off(item_t *UNUSED(item)) {
    power_off_pressed = !power_off_pressed;
    daemon_log(LOG_INFO, "power_of_off_icon clicked");
}

SDL_Texture *img_main_power_button_update(SDL_Renderer *renderer, struct ITEM_T *_item) {
    img_item_t *item = _item->custom_data;
    if (!item || !item->surface) {
        return NULL;
    }
    static bool first_time = true;
    if (first_time) {
        first_time = false;
        return colorizeTexture(renderer, item->surface, rgba_green);
    }
    static bool power_off_pressed_prev = false;
    static float c = 0.0f;

    if (power_off_pressed != power_off_pressed_prev) {
        power_off_pressed_prev = power_off_pressed;
        if (!power_off_pressed) {
            c = 0.0f;
            return colorizeTexture(renderer, item->surface, rgba_green);
        } else {
            c = 0.1f;
        }
    } else {
        if (power_off_pressed) {
            static time_t last_time = 0;
            time_t now;
            time(&now);
            if (last_time != now) {
                last_time = now;
                c += 0.1;
                if (c > 1.0) {
                    static bool first_time = true;
                    if (first_time) {
                        first_time = false;
                        daemon_log(LOG_INFO, "shutdown ret: %d", system("sudo shutdown -P -h now"));
                    }
                    return NULL;
                }
                return colorizeTexture(renderer, item->surface, lerp_color(rgba_green, rgba_red, c));
            }
        }
    }
    return NULL;
}

SDL_Texture *img_front_door_update(SDL_Renderer *renderer, struct ITEM_T *_item) {
    img_item_t *item = _item->custom_data;
    if (!item || !item->surface) {
        return NULL;
    }
    if (door.changed) {
        door.changed = false;
        if (!door.online) {
            return colorizeTexture(renderer, item->surface, rgba_grey);
        } else {
            if (door.open) {
                return colorizeTexture(renderer, item->surface, rgba_red);
            } else {
                return colorizeTexture(renderer, item->surface, rgba_green);
            }
        }
    }
    return NULL;
}

typedef enum battery_state_t {
    BATTERY_STATE_UNKNOWN,
    BATTERY_STATE_CHARGING,
    BATTERY_STATE_FULL,
    BATTERY_STATE_DISCHARGING_0,
    BATTERY_STATE_DISCHARGING_1,
    BATTERY_STATE_DISCHARGING_2,
    BATTERY_STATE_DISCHARGING_3,
    BATTERY_STATE_DISCHARGING_4,
    BATTERY_STATE_DISCHARGING_5,
    BATTERY_STATE_DISCHARGING_6,
    BATTERY_STATE_NONE,
} battery_state_t;

const char *battery_state_picture[] = {
        [BATTERY_STATE_UNKNOWN] = "/home/palich/bin/outline_battery_unknown_black_24.png",
        [BATTERY_STATE_CHARGING] = "/home/palich/bin/outline_battery_charging_full_black_24.png",
        [BATTERY_STATE_FULL] = "/home/palich/bin/outline_battery_full_black_24.png",
        [BATTERY_STATE_DISCHARGING_0] = "/home/palich/bin/outline_battery_0_bar_black_24.png",
        [BATTERY_STATE_DISCHARGING_1] = "/home/palich/bin/outline_battery_1_bar_black_24.png",
        [BATTERY_STATE_DISCHARGING_2] = "/home/palich/bin/outline_battery_2_bar_black_24.png",
        [BATTERY_STATE_DISCHARGING_3] = "/home/palich/bin/outline_battery_3_bar_black_24.png",
        [BATTERY_STATE_DISCHARGING_4] = "/home/palich/bin/outline_battery_4_bar_black_24.png",
        [BATTERY_STATE_DISCHARGING_5] = "/home/palich/bin/outline_battery_5_bar_black_24.png",
        [BATTERY_STATE_DISCHARGING_6] = "/home/palich/bin/outline_battery_6_bar_black_24.png",
};

battery_state_t get_battery_state(void) {
    if (isnan(battery.soc) || !battery.online) return BATTERY_STATE_UNKNOWN;
    if (battery.current > 0.0) return BATTERY_STATE_CHARGING;
    if (battery.current < 0.1) {
        if (battery.soc < 10.0) return BATTERY_STATE_DISCHARGING_0;
        if (battery.soc < 25.0) return BATTERY_STATE_DISCHARGING_1;
        if (battery.soc < 40.0) return BATTERY_STATE_DISCHARGING_2;
        if (battery.soc < 55.0) return BATTERY_STATE_DISCHARGING_3;
        if (battery.soc < 70.0) return BATTERY_STATE_DISCHARGING_4;
        if (battery.soc < 85.0) return BATTERY_STATE_DISCHARGING_5;
        if (battery.soc < 95.0) return BATTERY_STATE_DISCHARGING_6;
    }
    return BATTERY_STATE_FULL;
}

SDL_Texture *img_main_battery_update(SDL_Renderer *renderer, struct ITEM_T *_item) {
    img_item_t *item = _item->custom_data;
    if (!item || !item->surface) {
        return NULL;
    }
    static battery_state_t last_state = BATTERY_STATE_NONE;
    battery_state_t state = get_battery_state();
    if (state != last_state) {
        daemon_log(LOG_INFO, "battery state changed: %d", state);
        last_state = state;
        img_destroy(item);
        _item->custom_data = img_create(battery_state_picture[state]);
        item = _item->custom_data;
        if (isnan(battery.soc)) {
            return colorizeTexture(renderer, item->surface, rgba_grey);
        } else if (battery.soc < 20) {
            return colorizeTexture(renderer, item->surface, rgba_red);
        } else if (battery.soc < 50) {
            return colorizeTexture(renderer, item->surface, rgba_yellow);
        } else {
            return colorizeTexture(renderer, item->surface, rgba_green);
        }
    }
    return NULL;
}

item_t *root = NULL;

void init_textures(SDL_Renderer *renderer) {
    int screenWidth, screenHeight;
    SDL_GetRendererOutputSize(renderer, &screenWidth, &screenHeight);
    printf("screenWidth: %d, screenHeight: %d\n", screenWidth, screenHeight);

    {
        SDL_Point pos = {screenWidth / 2, screenHeight / 2};
        align_t align = {ALIGN_H_CENTER, ALIGN_V_CENTER};
        item_add(&root, item_new("time, ", renderer, pos, align, time_create(), time_update));
    }

    {
        SDL_Point pos = {screenWidth / 2, screenHeight / 3};
        align_t align = {ALIGN_H_CENTER, ALIGN_V_CENTER};
        item_add(&root, item_new("battery", renderer, pos, align, battery_create(), battery_update));
    }

    {
        SDL_Point pos = {screenWidth / 2, screenHeight * 3 / 4};
        align_t align = {ALIGN_H_CENTER, ALIGN_V_CENTER};
        item_add(&root, item_new("power", renderer, pos, align, power_create(), power_update));
    }

    {
        SDL_Point pos = {screenWidth / 3 - 90, screenHeight * 3 / 4 - 50};
        align_t align = {ALIGN_H_CENTER, ALIGN_V_CENTER};
        item_add(&root, item_new("indoor temp", renderer, pos, align, indoor_temp_create(), indoor_temp_update));
    }

    {
        SDL_Point pos = {screenWidth * 2 / 3 + 90, screenHeight * 3 / 4 - 50};
        align_t align = {ALIGN_H_CENTER, ALIGN_V_CENTER};
        item_add(&root, item_new("outdoor temp", renderer, pos, align, outdoor_temp_create(), outdoor_temp_update));
    }

    {
        SDL_Point pos = {20, 20};

        align_t align = {ALIGN_LEFT, ALIGN_TOP};
        item_t *icon = item_new("power_green_icon", renderer, pos, align,
                                img_create("/home/palich/bin/outline_power_black_24dp.png"),
                                img_main_power_update);

        item_add(&root, icon);

        int textureWidth, textureHeight;
        SDL_QueryTexture(icon->texture, NULL, NULL, &textureWidth, &textureHeight);

        pos.x = +20 + textureWidth;
        pos.y = 20;

        icon = item_new("power red icon", renderer, pos, align,
                        img_create("/home/palich/bin/outline_power_off_black_24dp.png"),
                        img_main_power_update2);

        item_add(&root, icon);

        SDL_QueryTexture(icon->texture, NULL, NULL, &textureWidth, &textureHeight);

        pos.x += 20 + textureWidth;
        pos.y = 20;

        icon = item_new("battery icon", renderer, pos, align,
                        img_create("/home/palich/bin/outline_battery_charging_full_black_24dp.png"),
                        img_main_battery_update);

        item_add(&root, icon);

    }

    {
        SDL_Point pos = {screenWidth - 70, 20};

        align_t align = {ALIGN_LEFT, ALIGN_TOP};
        item_t *icon = item_new("power_of_off_icon", renderer, pos, align,
                                img_create("/home/palich/bin/outline_power_settings_new_black_24dp.png"),
                                img_main_power_button_update);
        if (icon) {
            icon->on_click = on_click_power_off;
        }
        item_add(&root, icon);

        int textureWidth, textureHeight;
        SDL_QueryTexture(icon->texture, NULL, NULL, &textureWidth, &textureHeight);

        pos = (SDL_Point) {screenWidth - 70 - textureWidth - 20, 20};

        align = (align_t) {ALIGN_LEFT, ALIGN_TOP};
        icon = item_new("door_icon", renderer, pos, align,
                        img_create("/home/palich/bin/outline_door_front_black_24dp.png"),
                        img_front_door_update);
        item_add(&root, icon);

    }
}

SDL_Texture *colorizeTexture(SDL_Renderer *renderer, const SDL_Surface *surface, SDL_Color c) {
    if (!surface) {
        return NULL;
    } else {
        int width = surface->w;
        int height = surface->h;

        SDL_Surface *modifiedSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);

        Uint32 color = SDL_MapRGBA(surface->format, c.r, c.g, c.b, c.a);

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                Uint32 pixel = ((Uint32 *) surface->pixels)[y * width + x];
                Uint8 r, g, b, a;
                SDL_GetRGBA(pixel, surface->format, &r, &g, &b, &a);
                if (a != 0) {
                    pixel = color;
                }
                ((Uint32 *) modifiedSurface->pixels)[y * width + x] = pixel;
            }
        }

        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, modifiedSurface);
        SDL_FreeSurface(modifiedSurface);
        return texture;
    }
}

bool make_textures(SDL_Renderer *renderer, item_t *head) {
    bool changed = false;

    while (head) {
        SDL_Texture *new_texture = head->update(renderer, head);
        if (new_texture) {
            SDL_DestroyTexture(head->texture);
            head->texture = new_texture;
            changed = true;
        }
        head = head->next;
    }

    return changed;
}

// Initialize SDL, create window and renderer.
unsigned short sdl_setup(struct superclock *sc) {
    // Initialize SDL.
    if (SDL_Init(MY_SDL_FLAGS))
        return 1;

    // Initialize TTF
    if (TTF_Init())
        return 2;

    // Created the SDL Window.
    sc->win = SDL_CreateWindow(TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sc->window_width,
                               sc->window_height, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    if (!sc->win)
        return 3;

    // Create the SDL Renderer.
    sc->rend = SDL_CreateRenderer(sc->win, -1, SDL_RENDERER_ACCELERATED);
    if (!sc->rend)
        return 4;

    int numDisplays = SDL_GetNumVideoDisplays();
    for (int displayIndex = 0; displayIndex < numDisplays; ++displayIndex) {
        SDL_DisplayMode mode;
        SDL_GetCurrentDisplayMode(displayIndex, &mode);

        printf("Display %d:\n", displayIndex);
        printf("Width: %d\nHeight: %d\n", mode.w, mode.h);
    }

    return 0;
}


// Create a user callback event.
Uint32 timer_show_time() {
    // Create a user event to call the game loop.
    SDL_Event event;
    event.type = SDL_USEREVENT;
    event.user.code = 1;
    event.user.data1 = 0;
    event.user.data2 = 0;
    SDL_PushEvent(&event);
    return 0;
}

// Release memory and null pointers before exiting.
void memory_release_exit(struct superclock *sc) {
    switch (sc->exit_status) {
        case 1:
            fprintf(stderr, "Error initializing SDL: %s\n", SDL_GetError());
            break;
        case 2:
            fprintf(stderr, "Error initializing TTF: %s\n", TTF_GetError());
            break;
        case 3:
            fprintf(stderr, "Error creating window: %s\n", SDL_GetError());
            break;
        case 4:
            fprintf(stderr, "Error creating renderer: %s\n", SDL_GetError());
            break;
        case 5:
            fprintf(stderr, "Error creating icon surface: %s\n", SDL_GetError());
            break;
        case 6:
            fprintf(stderr, "Populating rectv array went out of bounds!\n");
            break;
        case 7:
            fprintf(stderr, "Error creating font: %s\n", TTF_GetError());
            break;
        case 8:
            fprintf(stderr, "Error creating a surface: %s\n", SDL_GetError());
            break;
        case 9:
            fprintf(stderr, "Error creating a texture: %s\n", SDL_GetError());
            break;
        case 10:
            fprintf(stderr, "Error an array was not the expected length:\n");
            break;
        default:
            break;
    }

    SDL_DestroyRenderer(sc->rend);
    sc->rend = NULL;
    SDL_DestroyWindow(sc->win);
    sc->win = NULL;
    SDL_Quit();
    exit(sc->exit_status);
}

static bool brightness_available = false;

int brightnessInit(void) {
    const char *cmd[] = {"gpio -g mode 18 pwm", "gpio pwmc 100"};
    for (size_t i = 0; i < ARRAY_SIZE(cmd); i++) {
        int res = system(cmd[i]);
        if (res != 0) {
            printf("[%s:%d] %s Error: %d \n", __FUNCTION__, __LINE__, cmd[i], WEXITSTATUS(res));
            return res;
        }
        sleep(1);
    }
    brightness_available = true;
    brightnessSet(0);
    return 0;
}


static int brightness = -1;

int brightnessSet(int value) {
    if (!brightness_available) {
        return -1;
    }
    if (value < 0) {
        value = 0;
    }
    if (brightnessGet() != value) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "gpio -g pwm 18 %d", value);
        int res = system(cmd);
        if (res != 0) {
            printf("[%s:%d] Error: %d \n", __FUNCTION__, __LINE__, WEXITSTATUS(res));
            return res;
        }
        brightness = value;
    }
    return 0;
}

pthread_t brightness_thread = 0;

void *brightness_thread_func(void *arg) {
    const int steps = 10;
    int value = (long) arg;
    double step = (value - brightnessGet()) / (double) steps;
    int start = brightnessGet();
    for (int i = 0; i < steps; i++) {
        brightnessSet(start + (int) (step * i));
        usleep(10000);
    }
    brightnessSet(value);
    pthread_exit(NULL);
}

int brightnessSetTo(int value) {
    if (!brightness_available) {
        return -1;
    }
    if (brightnessGet() != value) {
        if (brightness_thread) {
            pthread_cancel(brightness_thread);
            pthread_join(brightness_thread, NULL);
        }
        pthread_create(&brightness_thread, NULL, brightness_thread_func, (void *) (long) value);
    }
    return 0;
}

int brightnessGet(void) {
    return brightness;
}

int brightnessDeinit(void) {
    if (!brightness_available) {
        return -1;
    }
    int res = system("gpio -g mode 18 out");
    if (res != 0) {
        printf("[%s:%d] Error: %d \n", __FUNCTION__, __LINE__, WEXITSTATUS(res));
        return res;
    }
    return 0;
}

int align_h(int position, int textureSize, align_h_t align) {
    switch (align) {
        case ALIGN_LEFT:
            return position;
        case ALIGN_H_CENTER:
            return position - textureSize / 2;
        case ALIGN_RIGHT:
            return position + textureSize;
    }
    return position;
}

int align_v(int position, int textureSize, align_v_t align) {
    switch (align) {
        case ALIGN_TOP:
            return position;
        case ALIGN_V_CENTER:
            return position - textureSize / 2;
        case ALIGN_DOWN:
            return position + textureSize;
    }
    return position;
}

void battery_cb(const struct mosquitto_message *msg) {
    json_object *jobj = json_tokener_parse(msg->payload);
    json_object *j_soc = NULL;
    json_object_object_get_ex(jobj, "soc", &j_soc);
    double soc = json_object_get_double(j_soc);
    if (soc != battery.soc) {
        battery.changed = true;
        battery.soc = soc;
    }
    json_object *j_current = NULL;
    json_object_object_get_ex(jobj, "current", &j_current);
    double current = json_object_get_double(j_current);
    if (current != battery.current) {
        battery.changed = true;
        battery.current = current;
    }
    json_object *j_voltage = NULL;
    json_object_object_get_ex(jobj, "voltage", &j_voltage);
    double voltage = json_object_get_double(j_voltage);
    if (voltage != battery.voltage) {
        battery.changed = true;
        battery.voltage = voltage;
    }
    json_object *j_temp = NULL;
    json_object_object_get_ex(jobj, "temp_tube", &j_temp);
    double temp = json_object_get_double(j_temp);
    if (temp != battery.temp) {
        battery.changed = true;
        battery.temp = temp;
    }
    json_object *j_capacity = NULL;
    json_object_object_get_ex(jobj, "capacity", &j_capacity);
    double capacity = json_object_get_double(j_capacity);
    if (capacity != battery.capacity) {
        battery.changed = true;
        battery.capacity = capacity;
    }
    daemon_log(LOG_INFO, "soc: %.0f%%, current: %.2fA, voltage: %.2fV, power:%.2fW temp: %.0fC capacity: %.0f", soc,
               current, voltage,
               current * voltage, temp, capacity);
    json_object_put(jobj);
}

//{"Time":"2023-11-06T13:36:55","SHT3X":{"Temperature":36.7,"Humidity":27.3},"PZEM004T":{"Total":8211.639,"Power":540,"Voltage":235,"Current":3.070},"TempUnit":"C"}

void main_power_cb(const struct mosquitto_message *msg) {
    json_object *jobj = json_tokener_parse(msg->payload);
    json_object *j_pzem = NULL;
    json_object_object_get_ex(jobj, "PZEM004T", &j_pzem);

    json_object *j_power = NULL;
    json_object_object_get_ex(j_pzem, "Power", &j_power);
    double power = json_object_get_double(j_power);
    if (power != main_power.power) {
        main_power.changed = true;
        main_power.power = power;
    }
    json_object *j_voltage = NULL;
    json_object_object_get_ex(j_pzem, "Voltage", &j_voltage);
    double voltage = json_object_get_double(j_voltage);
    if (main_power.voltage != voltage) {
        main_power.changed = true;
        main_power.voltage = voltage;
    }
    daemon_log(LOG_INFO, "power: %.0fW, voltage: %.0fV", main_power.power, main_power.voltage);
    json_object_put(jobj);
}

void main_power_lwt_cb(const struct mosquitto_message *msg) {
    daemon_log(LOG_INFO, "main_power_lwt: %.*s", msg->payloadlen, (char *) msg->payload);
    main_power.online = strcmp((char *) msg->payload, "Online") == 0;
}

void main_battery_lwt_cb(const struct mosquitto_message *msg) {
    daemon_log(LOG_INFO, "main_battery_lwt: %.*s", msg->payloadlen, (char *) msg->payload);
    battery.online = strcmp((char *) msg->payload, "Online") == 0;
}


//{"Time":"2023-11-08T14:55:49","IN":{"time": "2023-11-08 14:55:32","brand": "ODROID","model": "WB2","id": 0,"channel": 1,"battery": "OK","temperature_C": 25.47,"humidity": 53.48,"pressure": 984.9,"altitude": 329.2581,"uv_index": 0.01,"visible": 206,"ir": 30},"EX":{"time": "2023-11-08 14:55:36","brand": "OS","model": "Oregon-THGR122N","id": 249,"channel": 1,"battery_ok": 1,"temperature_C": 9.3,"humidity": 87}}
void outdoor_cb(const struct mosquitto_message *msg) {
    json_object *jobj = json_tokener_parse(msg->payload);
    json_object *j_in = NULL;
    json_object_object_get_ex(jobj, "EX", &j_in);
    json_object *j_temperature = NULL;
    json_object_object_get_ex(j_in, "temperature_C", &j_temperature);
    double temperature = json_object_get_double(j_temperature);
    if (temperature != weather.temperature_outdoor) {
        weather.temperature_outdoor_changed = true;
        weather.temperature_outdoor = temperature;
    }
    daemon_log(LOG_INFO, "outdoor temperature: %.1fC", weather.temperature_outdoor);
    json_object_put(jobj);
}

void outdoor_lwt_cb(const struct mosquitto_message *msg) {
    daemon_log(LOG_INFO, "outdoor_lwt: %.*s", msg->payloadlen, (char *) msg->payload);
    weather.temperature_outdoor_online = strcasecmp((char *) msg->payload, "Online") == 0;
}

// {"battery":100,"humidity":51.52,"last_seen":"2023-11-08T12:53:56.724Z","linkquality":76,"pressure":984.7,"temperature":23.39,"voltage":3005}
void thps_sf_hall_cb(const struct mosquitto_message *msg) {
    json_object *jobj = json_tokener_parse(msg->payload);
    json_object *j_temperature = NULL;
    json_object_object_get_ex(jobj, "temperature", &j_temperature);
    double temperature = json_object_get_double(j_temperature);
    if (temperature != weather.temperature_indoor) {
        weather.temperature_indoor_changed = true;
        weather.temperature_indoor = temperature;
    }
    daemon_log(LOG_INFO, "indoor temperature: %.1fC", weather.temperature_indoor);
    json_object_put(jobj);
}

void thps_sf_hall_lwt_cb(const struct mosquitto_message *msg) {
    weather.temperature_indoor_online = strcasecmp((char *) msg->payload, "Online") == 0;
}

void dos_entranse_lwt_cb(const struct mosquitto_message *msg) {
    door.online = strcasecmp((char *) msg->payload, "Online") == 0;
    door.changed = true;
}

void dos_entranse_cb(const struct mosquitto_message *msg) {
    json_object *root = json_tokener_parse(msg->payload);
    if (root) {
        json_object *j_contact = NULL;
        json_object_object_get_ex(root, "contact", &j_contact);
        bool contact = json_object_get_boolean(j_contact);
        if (!contact != door.open) {
            door.open = !contact;
            daemon_log(LOG_INFO, "door open: %d", door.open);
            door.changed = true;
        }
        json_object_put(root);
    }
}

item_t *detect_where_mouse_pressed(int x, int y) {
    for (item_t *item = root; item; item = item->next) {
        int textureWidth, textureHeight;

        SDL_QueryTexture(item->texture, NULL, NULL, &textureWidth, &textureHeight);

        SDL_Rect rect = {align_h(item->position.x, textureWidth, item->align.align_h),
                         align_v(item->position.y, textureHeight, item->align.align_v),
                         textureWidth,
                         textureHeight};
        if (SDL_PointInRect(&(SDL_Point) {x, y}, &rect)) {
            return item;
        }
    }
    return NULL;
}

#define HOSTNAME_SIZE 256
#define CDIR "./"

int main(int UNUSED(argc), char *const *argv) {
    SDL_Event event;
    SDL_TimerID timer;

    const char *progname = NULL;
    char *pathname = NULL;

    tzset();

    if ((progname = strrchr(argv[0], '/')) == NULL)
        progname = argv[0];
    else
        ++progname;

    if (strrchr(argv[0], '/') == NULL)
        pathname = xstrdup(CDIR);
    else {
        pathname = xmalloc(strlen(argv[0]) + 1);
        strncpy(pathname, argv[0], (size_t)(strrchr(argv[0], '/') - argv[0]) + 1);
    }

    if (chdir(pathname) < 0) {
        daemon_log(LOG_ERR, "chdir error: %s", strerror(errno));
    }

    FREE(pathname);

    pathname = get_current_dir_name();

    char *hostname = calloc(1, HOSTNAME_SIZE);
    gethostname(hostname, HOSTNAME_SIZE - 1);

    daemon_log_upto(LOG_INFO);
    daemon_log(LOG_INFO, "%s %s", pathname, progname);

    struct superclock sc = {
            .win = NULL,
            .rend = NULL,
            .window_width = 640,
            .window_height = 480,
            .running = true,
            .show_time = false,
    };


    // Initialize SDL, create window and renderer.

    sc.exit_status = sdl_setup(&sc);
    if (sc.exit_status)
        memory_release_exit(&sc);
    srandom(time(NULL));


    init_textures(sc.rend);
    SDL_ShowCursor(SDL_DISABLE);
    brightnessInit();

    mosq_register_on_message_cb("tele/main_battery/SENSOR", battery_cb);
    mosq_register_on_message_cb("tele/main-power/SENSOR", main_power_cb);
    mosq_register_on_message_cb("tele/main-power/LWT", main_power_lwt_cb);
    mosq_register_on_message_cb("tele/main_battery/LWT", main_battery_lwt_cb);
    mosq_register_on_message_cb("tele/hass/SENSOR", outdoor_cb);
    mosq_register_on_message_cb("tele/hass/LWT", outdoor_lwt_cb);
    mosq_register_on_message_cb("zigbee2mqtt/thps_sf_hall", thps_sf_hall_cb);
    mosq_register_on_message_cb("zigbee2mqtt/thps_sf_hall/availability", thps_sf_hall_lwt_cb);
    mosq_register_on_message_cb("zigbee2mqtt/dos-entranse/availability", dos_entranse_lwt_cb);
    mosq_register_on_message_cb("zigbee2mqtt/dos-entranse", dos_entranse_cb);

    mosq_init("superclock-sdl", hostname);

    time_t last_active = time(NULL);
    bool first = true;
    while (sc.running) {
        // Check key events, key pressed or released.
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    // handling of close button
                    sc.running = false;
                    break;
                case SDL_USEREVENT:
                    if (event.user.code == 1) {
                        sc.show_time = false;
                        SDL_SetWindowTitle(sc.win, TITLE);
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    //case SDL_MOUSEMOTION:
                    last_active = time(NULL);
                    brightnessSetTo(0);
                    item_t *item = detect_where_mouse_pressed(event.button.x, event.button.y);
                    if (item) {
                        daemon_log(LOG_INFO, "mouse button: %d x:%d y:%d item: %s", event.button.button,
                                   event.button.x,
                                   event.button.y, item->name);
                        if (item->on_click) {
                            item->on_click(item);
                        }
                    } else {
                        daemon_log(LOG_INFO, "mouse button: %d x:%d y:%d", event.button.button, event.button.x,
                                   event.button.y);

                    }
                    break;
                case SDL_KEYDOWN:
                    // keyboard API for key pressed
                    brightnessSetTo(0);
                    last_active = time(NULL);
                    switch (event.key.keysym.scancode) {
                        case SDL_SCANCODE_SPACE:
                            if (sc.show_time)
                                SDL_RemoveTimer(timer);
                            else
                                sc.show_time = true;
                            timer = SDL_AddTimer(5000, timer_show_time, NULL);
                            break;
                        default:
                            break;
                    }
                default:
                    break;
            }
        }

        if (first || make_textures(sc.rend, root)) {
            first = false;
            last_active = time(NULL);
            brightnessSetTo(0);
            SDL_RenderClear(sc.rend);

            SDL_SetRenderDrawColor(sc.rend, rgba_background.r, rgba_background.g, rgba_background.b, rgba_background.a);
            SDL_Rect rect = {0, 0, sc.window_width, sc.window_height};
            SDL_RenderDrawRect(sc.rend, &rect);
            SDL_RenderFillRect(sc.rend, &rect);

            SDL_SetRenderDrawColor(sc.rend, 28, 81, 128, 255);
            int offset = 4;
            SDL_Rect rect1 = {0 + offset, 0 + offset, sc.window_width - offset * 2, sc.window_height - offset * 2};
            SDL_RenderDrawRect(sc.rend, &rect1);
            SDL_RenderFillRect(sc.rend, &rect1);

            // Draw the images to the renderer.

            for (item_t *item = root; item; item = item->next) {
                int textureWidth, textureHeight;

                SDL_QueryTexture(item->texture, NULL, NULL, &textureWidth, &textureHeight);

                SDL_Rect rect = {align_h(item->position.x, textureWidth, item->align.align_h),
                                 align_v(item->position.y, textureHeight, item->align.align_v),
                                 textureWidth,
                                 textureHeight};

                SDL_RenderCopy(sc.rend, item->texture, NULL, &rect);
            }

            SDL_RenderPresent(sc.rend);
        } else {
            if (time(NULL) - last_active > 5) {
                brightnessSetTo(600);
            }
        }
        sleep(1);
    }
    brightnessDeinit();
    SDL_ShowCursor(SDL_ENABLE);
    memory_release_exit(&sc);
    mosq_destroy();
}