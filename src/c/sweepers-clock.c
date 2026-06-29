#include <pebble.h>

// "Sweeper's Clock" — after Maarten Baas' "Real Time" series.
//
// Each grain of garbage has a fixed identity (radius along the hand, sideways
// offset, colour) and its own stored angle. The clock's target angle creeps
// clockwise with real time, but a grain NEVER moves on its own: it only advances
// (clockwise, toward the target) while a sweeper's broom is touching it. So the
// hands are formed and advanced purely by the slow work of the two sweepers.

// Geometry is resolved from the display size at load (gabbro is 260x260 round,
// emery is 200x228) — see resolve_geometry().
static GPoint s_center;
static int s_radius;    // min(w, h) / 2
static int s_min_len;   // minute hand length
static int s_hour_len;  // hour hand length
#define CENTER s_center
#define RADIUS s_radius
#define MIN_LEN s_min_len
#define HOUR_LEN s_hour_len

#define MIN_START 0            // hands meet in the middle
#define MIN_COUNT 150
#define MIN_SEED 0x2000u

#define HOUR_START 0
#define HOUR_COUNT 120
#define HOUR_SEED 0x0100u

#define PILE_SPREAD 7
#define FRAME_MS 33            // ~30 fps animation
#define DISTURB_R2 (6 * 6)     // grains only move within the broom's bristle reach
#define PUSH_MIN 24            // minimum clockwise nudge per touched frame

// Human sweep stroke: plant broom, push forward a short clockwise arc, lift,
// step back, repeat — slowly drifting along the hand's length.
#define STROKE 64              // frames per stroke (push + lift-and-step-back)
#define STROKE_PUSH 40         // of which this many are the forward push
#define STROKE_ARC (TRIG_MAX_ANGLE / 40)  // ~9 deg pushed per stroke
#define RAD_PERIOD 1500        // frames to drift once along the hand (~50s)

// Sweeper B's slow tour: dwell on the minute hand, walk to the hour hand
// (through the center), dwell, walk back. All frame counts at ~30 fps.
#define B_MIN_DWELL 2400       // ~80s working the minute hand
#define B_TRAVEL 120           // ~4s walking each way (broom up)
#define B_HOUR_DWELL 600       // ~20s tending the hour hand

static Window *s_window;
static Layer *s_layer;
static AppTimer *s_timer;
static uint32_t s_frame;

// --- hashing & a tiny PRNG (PRNG only used for the static floor texture) -----
static uint32_t hash2(uint32_t a, uint32_t b) {
  uint32_t h = a * 2654435761u ^ (b + 0x9e3779b9u) * 40503u;
  h ^= h >> 13;
  h *= 0x85ebca6bu;
  h ^= h >> 16;
  return h;
}

static uint32_t s_rng;
static void rng_seed(uint32_t s) { s_rng = s ? s : 0xA53C9E1u; }
static uint32_t rng_next(void) {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}
static int rng_range(int lo, int hi) {
  return lo + (int)(rng_next() % (uint32_t)(hi - lo + 1));
}

// Near-white "trash" tints so the hands stand out against black.
static const uint8_t s_debris_argb[] = {
  0b11111111,  // white
  0b11111110,  // FFFFAA cream
  0b11111101,  // FFFF55 light yellow
  0b11111011,  // FFAAFF light pink
  0b11111010,  // FFAAAA light salmon
  0b11101111,  // AAFFFF light cyan
  0b11111111,  // white (weighted)
  0b11101011,  // AAAAFF light blue
};
#define DEBRIS_N (sizeof(s_debris_argb))

typedef struct {
  uint16_t angle;  // current angular position (0..TRIG_MAX_ANGLE-1)
  uint8_t t;       // radius along the hand
  int8_t perp;     // sideways offset from the spine
  uint8_t color;   // index into s_debris_argb
  uint8_t clump;   // draw as a tiny clump rather than a single pixel
} Grain;

static Grain s_min[MIN_COUNT];
static Grain s_hour[HOUR_COUNT];
static bool s_inited;

static void init_pile(Grain *g, int count, uint32_t seed, int start, int len,
                      uint16_t angle0) {
  for (int i = 0; i < count; i++) {
    uint32_t h = hash2(seed, (uint32_t)i);
    int p1 = (int)((h >> 5) % (2 * PILE_SPREAD + 1)) - PILE_SPREAD;
    int p2 = (int)((h >> 11) % (2 * PILE_SPREAD + 1)) - PILE_SPREAD;
    g[i].t = (uint8_t)(start + (int)(h % (uint32_t)(len - start + 1)));
    g[i].perp = (int8_t)((p1 + p2) / 2);
    g[i].color = (uint8_t)((h >> 18) % DEBRIS_N);
    g[i].clump = (uint8_t)(((h >> 23) % 5u) == 0u);
    g[i].angle = angle0;
  }
}

static GPoint grain_pt(const Grain *g) {
  int32_t s = sin_lookup(g->angle);
  int32_t c = cos_lookup(g->angle);
  int32_t bx = CENTER.x + (s * g->t) / TRIG_MAX_RATIO;
  int32_t by = CENTER.y - (c * g->t) / TRIG_MAX_RATIO;
  int px = (int)bx + (int)((c * g->perp) / TRIG_MAX_RATIO);
  int py = (int)by + (int)((s * g->perp) / TRIG_MAX_RATIO);
  return GPoint(px, py);
}

// A grain only moves while a broom is touching it, and only clockwise toward
// the target angle (never past it, never on its own).
static void sweep_grain(Grain *g, uint16_t target, const GPoint *brooms, int nb) {
  GPoint p = grain_pt(g);
  bool touched = false;
  for (int k = 0; k < nb; k++) {
    int dx = p.x - brooms[k].x, dy = p.y - brooms[k].y;
    if (dx * dx + dy * dy < DISTURB_R2) { touched = true; break; }
  }
  if (!touched) return;

  uint16_t fwd = (uint16_t)(target - g->angle);  // clockwise distance to target
  if (fwd == 0 || fwd >= TRIG_MAX_ANGLE / 2) return;  // at/ahead of target
  uint16_t stepv = (uint16_t)(fwd / 4 + PUSH_MIN);
  if (stepv > fwd) stepv = fwd;
  g->angle = (uint16_t)(g->angle + stepv);
}

static void draw_grains(GContext *ctx, const Grain *g, int n) {
  for (int i = 0; i < n; i++) {
    GPoint p = grain_pt(&g[i]);
    GColor col = (GColor){ .argb = s_debris_argb[g[i].color] };
    if (g[i].clump) {
      graphics_context_set_fill_color(ctx, col);
      graphics_fill_circle(ctx, p, 1);
    } else {
      graphics_context_set_stroke_color(ctx, col);
      graphics_draw_pixel(ctx, p);
    }
  }
}

// The state of one human sweep stroke at the current frame.
typedef struct {
  GPoint broom;   // where the broom head is
  int32_t face;   // angle the worker is pushing toward (clockwise tangential)
  bool down;      // true while pushing forward (the only time grains move)
} Stroke;

// A sweeper plants the broom STROKE_ARC behind the target and pushes forward to
// it, then lifts and steps back. It also drifts slowly along the hand's length.
static Stroke stroke_state(int32_t target, int start, int len, uint32_t frame,
                           uint32_t phase) {
  uint32_t p = (frame + phase) % STROKE;
  bool down = p < STROKE_PUSH;
  int off = down ? (int)((p * STROKE_ARC) / STROKE_PUSH)
                 : (int)(((STROKE - p) * STROKE_ARC) / (STROKE - STROKE_PUSH));
  int32_t ba = target - STROKE_ARC + off;  // sweeps from behind up to target

  int32_t s = sin_lookup(ba), c = cos_lookup(ba);
  uint32_t rp = (frame + phase) % RAD_PERIOD;
  int half = RAD_PERIOD / 2;
  int upr = ((int)rp < half) ? (int)rp : (RAD_PERIOD - (int)rp);
  int r = start + (upr * (len - start)) / half;

  Stroke st;
  st.broom = GPoint(CENTER.x + (int)((s * r) / TRIG_MAX_RATIO),
                    CENTER.y - (int)((c * r) / TRIG_MAX_RATIO));
  st.face = ba;
  st.down = down;
  return st;
}

// Draw the worker given resolved body & broom-head points. When `down`, the
// broom reaches the pile; otherwise it is lifted (the worker is walking).
static void draw_sprite(GContext *ctx, GPoint body, GPoint broom, bool down,
                        uint32_t frame) {
  int step = (int)((frame >> 3) & 1u);
  int bx = body.x, by = body.y - step;

  int hx, hy;
  if (down) {
    hx = broom.x;
    hy = broom.y;
  } else {                          // broom raised while walking
    hx = bx + (broom.x - bx) / 2;
    hy = by + (broom.y - by) / 2 - 3;
  }

  int hdx = hx - bx, hdy = hy - by;
  graphics_context_set_stroke_color(ctx, GColorFromRGB(0xAA, 0x55, 0x00));
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(bx, by), GPoint(hx, hy));

  // bristle fan perpendicular to the handle, splayed forward onto the trash
  // bristle fan as wide as the sweep reach, so the brush covers what it moves
  int L = (hdx < 0 ? -hdx : hdx) + (hdy < 0 ? -hdy : hdy);
  if (L < 1) L = 1;
  graphics_context_set_stroke_color(ctx, GColorFromRGB(0xFF, 0xFF, 0xAA));
  for (int k = -7; k <= 7; k++) {
    int fx = hx + (-hdy * k) / L;
    int fy = hy + (hdx * k) / L;
    int gx = fx + (hdx * 2) / L;
    int gy = fy + (hdy * 2) / L;
    graphics_draw_line(ctx, GPoint(fx, fy), GPoint(gx, gy));
  }

  graphics_context_set_fill_color(ctx, GColorFromRGB(0x00, 0x00, 0xAA));
  graphics_fill_circle(ctx, GPoint(bx, by), 3);
  graphics_context_set_fill_color(ctx, GColorFromRGB(0xFF, 0xAA, 0x55));
  graphics_fill_circle(ctx, GPoint(bx, by - 3), 2);

  graphics_context_set_stroke_color(ctx, GColorFromRGB(0x00, 0x00, 0x55));
  if (step) {
    graphics_draw_pixel(ctx, GPoint(bx - 3, by + 1));
    graphics_draw_pixel(ctx, GPoint(bx + 2, by + 3));
  } else {
    graphics_draw_pixel(ctx, GPoint(bx - 2, by + 3));
    graphics_draw_pixel(ctx, GPoint(bx + 3, by + 1));
  }
}

// Resolve a sweeping worker's body point sitting one pace behind the broom in
// the push direction.
static GPoint body_behind(GPoint broom, int32_t face) {
  int32_t s = sin_lookup(face), c = cos_lookup(face);
  return GPoint(broom.x - (int)((c * 9) / TRIG_MAX_RATIO),
                broom.y - (int)((s * 9) / TRIG_MAX_RATIO));
}

static void draw_floor(GContext *ctx, GRect bounds) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // faint, constant dark dusting so the floor isn't a flat void
  rng_seed(0x1234ABCDu);
  graphics_context_set_stroke_color(ctx, GColorFromRGB(0x55, 0x55, 0x55));
  for (int i = 0; i < 70; i++) {
    int x = rng_range(0, bounds.size.w - 1);
    int y = rng_range(0, bounds.size.h - 1);
    if (rng_next() & 1) graphics_draw_pixel(ctx, GPoint(x, y));
  }
}

static void update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  draw_floor(ctx, bounds);

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int sec = t->tm_sec, min = t->tm_min, hr = t->tm_hour % 12;

  // Continuous clockwise targets. The minute hand creeps a full turn per hour;
  // the hour hand a full turn per 12 hours (both very gradual).
  uint16_t min_target = (uint16_t)((TRIG_MAX_ANGLE * (min * 60 + sec)) / 3600);
  uint16_t hour_target = (uint16_t)(((uint32_t)TRIG_MAX_ANGLE *
                                     ((uint32_t)hr * 3600 + min * 60 + sec)) /
                                    43200);

  if (!s_inited) {
    init_pile(s_min, MIN_COUNT, MIN_SEED, MIN_START, MIN_LEN, min_target);
    init_pile(s_hour, HOUR_COUNT, HOUR_SEED, HOUR_START, HOUR_LEN, hour_target);
    s_inited = true;
  }

  // Sweeper A always works the minute hand.
  Stroke A = stroke_state(min_target, MIN_START, MIN_LEN, s_frame, 0);
  GPoint a_body = body_behind(A.broom, A.face);

  // Sweeper B works the minute hand, then tours to the hour hand and back on a
  // slow cycle, walking through the center (where the hands meet) so it never
  // jumps. w: 0 = on minute, 256 = on hour, in between = walking.
  uint32_t Bphase = STROKE / 2 + 211;
  Stroke Bm = stroke_state(min_target, MIN_START, MIN_LEN, s_frame, Bphase);
  Stroke Bh = stroke_state(hour_target, HOUR_START, HOUR_LEN, s_frame, Bphase);

  uint32_t total = B_MIN_DWELL + B_TRAVEL + B_HOUR_DWELL + B_TRAVEL;
  uint32_t cyc = s_frame % total;
  int w;
  if (cyc < B_MIN_DWELL) {
    w = 0;
  } else if (cyc < B_MIN_DWELL + B_TRAVEL) {
    w = (int)((cyc - B_MIN_DWELL) * 256 / B_TRAVEL);
  } else if (cyc < B_MIN_DWELL + B_TRAVEL + B_HOUR_DWELL) {
    w = 256;
  } else {
    w = 256 - (int)((cyc - (B_MIN_DWELL + B_TRAVEL + B_HOUR_DWELL)) * 256 / B_TRAVEL);
  }

  GPoint b_broom, b_body;
  bool b_down, b_on_min = false, b_on_hour = false;
  if (w <= 0) {
    b_broom = Bm.broom;
    b_body = body_behind(b_broom, Bm.face);
    b_down = Bm.down;
    b_on_min = true;
  } else if (w >= 256) {
    b_broom = Bh.broom;
    b_body = body_behind(b_broom, Bh.face);
    b_down = Bh.down;
    b_on_hour = true;
  } else {  // walking through the center, broom up
    GPoint from, to;
    int frac;
    if (w < 128) { from = Bm.broom; to = CENTER; frac = w * 2; }
    else { from = CENTER; to = Bh.broom; frac = (w - 128) * 2; }
    b_broom = GPoint(from.x + (to.x - from.x) * frac / 256,
                     from.y + (to.y - from.y) * frac / 256);
    int dx = to.x - from.x, dy = to.y - from.y;
    int L = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (L < 1) L = 1;
    b_body = GPoint(b_broom.x - dx * 9 / L, b_broom.y - dy * 9 / L);
    b_down = false;
  }

  // Only brooms that are pushing (down) actually move grains.
  GPoint min_brooms[2];
  int n_min = 0;
  if (A.down) min_brooms[n_min++] = A.broom;
  if (b_on_min && b_down) min_brooms[n_min++] = b_broom;

  GPoint hour_brooms[1];
  int n_hour = 0;
  if (b_on_hour && b_down) hour_brooms[n_hour++] = b_broom;

  for (int i = 0; i < MIN_COUNT; i++)
    sweep_grain(&s_min[i], min_target, min_brooms, n_min);
  for (int i = 0; i < HOUR_COUNT; i++)
    sweep_grain(&s_hour[i], hour_target, hour_brooms, n_hour);

  draw_grains(ctx, s_hour, HOUR_COUNT);
  draw_grains(ctx, s_min, MIN_COUNT);

  draw_sprite(ctx, b_body, b_broom, b_down, s_frame);
  draw_sprite(ctx, a_body, A.broom, A.down, s_frame);
}

static void anim_timer(void *data) {
  s_frame++;
  layer_mark_dirty(s_layer);
  s_timer = app_timer_register(FRAME_MS, anim_timer, NULL);
}

static void resolve_geometry(GRect bounds) {
  s_center = grect_center_point(&bounds);
  int w = bounds.size.w, h = bounds.size.h;
  s_radius = (w < h ? w : h) / 2;
  s_min_len = s_radius * 80 / 100;   // ~104 on gabbro, ~80 on emery
  s_hour_len = s_radius * 54 / 100;  // ~70 on gabbro, ~54 on emery
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  resolve_geometry(bounds);
  s_layer = layer_create(bounds);
  layer_set_update_proc(s_layer, update_proc);
  layer_add_child(root, s_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_layer);
}

static void init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .unload = window_unload,
                                       });
  window_stack_push(s_window, true);
  s_timer = app_timer_register(FRAME_MS, anim_timer, NULL);
}

static void deinit(void) {
  if (s_timer) app_timer_cancel(s_timer);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
