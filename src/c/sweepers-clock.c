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
#define STROKE_ARC (TRIG_MAX_ANGLE / 40)  // ~9 deg minimum push per stroke
#define RAD_PERIOD 1500        // frames to drift once along the hand (~50s)
// The broom plants far enough behind to reach the worst straggler, so a grain
// can never fall permanently out of reach. Capped so recovery sweeps stay sane.
#define ARC_MARGIN (TRIG_MAX_ANGLE / 120)  // ~3 deg behind the worst laggard
#define MAX_ARC (TRIG_MAX_ANGLE / 6)       // ~60 deg cap on reach-back

// How far (perpendicular) the worker's body stands off the pile line — a fixed
// offset so the (now larger) figure never sits on the pile, while only the
// broom does the slow sweeping.
#define STANDOFF 22

// Pauses: a worker doesn't sweep every stroke. It rests a fraction of stroke
// slots (standing back, observing), and rests more when little needs pushing.
// The two use different rest patterns, so often only one works at a time.
#define REST_BASE 64    // % of slots spent resting when the hand is caught up
#define REST_SPAN 52    // rest less as more grains lag behind
#define LAG_THRESH 320  // a grain this far (angle units) behind target = "work"

// Sweeper B's slow tour: dwell on the minute hand, walk to the hour hand
// (through the center), dwell, walk back. All frame counts at ~30 fps.
#define B_MIN_DWELL 2400      // ~80s working the minute hand
#define B_TRAVEL 120           // ~4s walking each way (broom up)
#define B_HOUR_DWELL 600       // ~20s tending the hour hand

static Window *s_window;
static Layer *s_layer;
static AppTimer *s_timer;
static uint32_t s_frame;

static int isqrt(int v) {
  if (v <= 0) return 0;
  int x = v, y = (x + 1) / 2;
  while (y < x) { x = y; y = (x + v / x) / 2; }
  return x;
}

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
  GPoint broom;   // where the broom head reaches (on the pile, when down)
  GPoint body;    // where the worker stands (stepped off the line)
  int32_t face;   // angle the worker is pushing toward (clockwise tangential)
  bool down;      // true while pushing forward (the only time grains move)
  bool active;    // false while resting (standing back, observing)
} Stroke;

// A sweeper plants the broom `plant_arc` behind the target and pushes forward to
// it, then lifts and drifts slowly along the hand. `plant_arc` grows to reach
// the worst straggler so nothing is left behind. When `resting` it just holds
// the planted pose and watches — no push, so no grains move. The body always
// stands exactly 90 degrees off the hand, so the broom pushes side-on.
static Stroke stroke_state(int32_t target, int start, int len, uint32_t frame,
                           uint32_t phase, bool resting, int plant_arc) {
  uint32_t p = (frame + phase) % STROKE;
  bool down = !resting && p < STROKE_PUSH;
  int off = resting ? 0
            : (down ? (int)((p * plant_arc) / STROKE_PUSH)
                    : (int)(((STROKE - p) * plant_arc) / (STROKE - STROKE_PUSH)));
  int32_t ba = target - plant_arc + off;  // broom head sweeps the arc up to target

  int32_t s = sin_lookup(ba), c = cos_lookup(ba);
  uint32_t rp = (frame + phase) % RAD_PERIOD;
  int half = RAD_PERIOD / 2;
  int upr = ((int)rp < half) ? (int)rp : (RAD_PERIOD - (int)rp);
  int r = start + (upr * (len - start)) / half;

  // The hand's own perpendicular (tangential) direction — the body stands off
  // along it so the broom handle is exactly 90 degrees to the hand.
  int32_t ct = cos_lookup(target), stg = sin_lookup(target);

  Stroke st;
  st.broom = GPoint(CENTER.x + (int)((s * r) / TRIG_MAX_RATIO),
                    CENTER.y - (int)((c * r) / TRIG_MAX_RATIO));
  st.body = GPoint(st.broom.x - (int)((ct * STANDOFF) / TRIG_MAX_RATIO),
                   st.broom.y - (int)((stg * STANDOFF) / TRIG_MAX_RATIO));
  st.face = ba;
  st.down = down;
  st.active = !resting;
  return st;
}

// Draw a worker (~3x the old size): an upright little figure in blue overalls
// holding a broom that reaches toward `broom`. When `down` the broom is on the
// pile; otherwise it is lifted and carried while walking.
static void draw_sprite(GContext *ctx, GPoint body, GPoint broom, bool down,
                        bool moving, uint32_t frame) {
  int swing = moving ? (int)((frame >> 2) & 3u) : 0;  // legs only move when moving
  int legA = (swing == 1) ? 1 : (swing == 3) ? -1 : 0;
  int bob = (swing == 1 || swing == 3) ? -1 : 0;
  int bx = body.x, by = body.y + bob;

  const GColor overalls = GColorFromRGB(0x00, 0x00, 0xAA);
  const GColor skin = GColorFromRGB(0xFF, 0xAA, 0x55);
  const GColor wood = GColorFromRGB(0xAA, 0x55, 0x00);
  const GColor straw = GColorFromRGB(0xFF, 0xFF, 0xAA);
  const GColor dark = GColorFromRGB(0x00, 0x00, 0x55);

  // --- broom: from the figure's hands toward the pile -----------------------
  int dx = broom.x - bx, dy = broom.y - by;
  int len = isqrt(dx * dx + dy * dy);
  if (len < 1) len = 1;
  int ux = dx * 256 / len, uy = dy * 256 / len;  // unit toward pile (x256)
  int grip_x = bx + ux * 7 / 256, grip_y = by + uy * 7 / 256 - 2;
  int hx, hy;
  if (down) {                                  // broom head on the pile
    hx = broom.x;
    hy = broom.y;
  } else {                                     // lifted and carried
    hx = bx + dx * 55 / 100;
    hy = by + dy * 55 / 100 - 6;
  }

  // legs (walking)
  graphics_context_set_stroke_color(ctx, dark);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(bx - 2, by + 7), GPoint(bx - 3 + legA, by + 14));
  graphics_draw_line(ctx, GPoint(bx + 2, by + 7), GPoint(bx + 3 - legA, by + 14));

  // broom handle
  graphics_context_set_stroke_color(ctx, wood);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(grip_x, grip_y), GPoint(hx, hy));

  // bristle fan, perpendicular to the broom, splayed onto the trash
  int px = -uy, py = ux;  // perpendicular unit (x256)
  graphics_context_set_stroke_color(ctx, straw);
  graphics_context_set_stroke_width(ctx, 1);
  for (int k = -9; k <= 9; k += 2) {
    int fx = hx + px * k / 256, fy = hy + py * k / 256;
    int gx = fx + ux * 5 / 256, gy = fy + uy * 5 / 256;
    graphics_draw_line(ctx, GPoint(fx, fy), GPoint(gx, gy));
  }

  // torso (overalls) + head
  graphics_context_set_fill_color(ctx, overalls);
  graphics_fill_rect(ctx, GRect(bx - 4, by - 3, 9, 11), 3, GCornersAll);
  graphics_context_set_fill_color(ctx, skin);
  graphics_fill_circle(ctx, GPoint(bx, by - 7), 4);

  // arm to the broom grip
  graphics_context_set_stroke_color(ctx, overalls);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(bx, by - 1), GPoint(grip_x, grip_y));
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

static GPoint polar(int radius, int32_t angle) {
  int32_t s = sin_lookup(angle), c = cos_lookup(angle);
  return GPoint(CENTER.x + (int)((s * radius) / TRIG_MAX_RATIO),
                CENTER.y - (int)((c * radius) / TRIG_MAX_RATIO));
}

static GPoint lerp_pt(GPoint a, GPoint b, int t256) {
  return GPoint(a.x + (b.x - a.x) * t256 / 256, a.y + (b.y - a.y) * t256 / 256);
}

// Walking from the minute hand to the hour hand WITHOUT crossing either pile:
// step out past the tips, arc around the rim, then step in. frac 0..256.
static GPoint travel_point(int frac, GPoint mb, GPoint hb, int32_t mt,
                           int32_t ht, int router) {
  GPoint out_m = polar(router, mt), out_h = polar(router, ht);
  if (frac < 85) return lerp_pt(mb, out_m, frac * 256 / 85);
  if (frac < 171) {
    int16_t d = (int16_t)(ht - mt);  // short way around the outside
    int32_t a = mt + (int32_t)d * (frac - 85) / 86;
    return polar(router, a);
  }
  return lerp_pt(out_h, hb, (frac - 171) * 256 / 85);
}

// How many grains are still meaningfully behind the target (i.e. need pushing).
static int count_lag(const Grain *g, int n, uint16_t target) {
  int lag = 0;
  for (int i = 0; i < n; i++) {
    uint16_t fwd = (uint16_t)(target - g[i].angle);
    if (fwd > LAG_THRESH && fwd < TRIG_MAX_ANGLE / 2) lag++;
  }
  return lag;
}

// More lag -> rest less; caught up -> rest more.
static int rest_pct(int lag, int total) {
  int p = REST_BASE - (REST_SPAN * lag) / total;
  return p < 6 ? 6 : p;
}

// The largest amount any grain is behind the target (clockwise distance).
static uint16_t pile_max_lag(const Grain *g, int n, uint16_t target) {
  uint16_t m = 0;
  for (int i = 0; i < n; i++) {
    uint16_t fwd = (uint16_t)(target - g[i].angle);
    if (fwd < TRIG_MAX_ANGLE / 2 && fwd > m) m = fwd;
  }
  return m;
}

// Plant the broom just behind the worst straggler (>= the normal stroke arc,
// <= a sane cap), so every grain is always within reach of a sweep.
static int plant_arc_for(uint16_t max_lag) {
  int a = (int)max_lag + ARC_MARGIN;
  if (a < STROKE_ARC) a = STROKE_ARC;
  if (a > MAX_ARC) a = MAX_ARC;
  return a;
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

  // Rest probability per hand: more when the hand is already mostly formed.
  int min_rest = rest_pct(count_lag(s_min, MIN_COUNT, min_target), MIN_COUNT);
  int hour_rest = rest_pct(count_lag(s_hour, HOUR_COUNT, hour_target), HOUR_COUNT);

  // Reach-back per hand: enough to catch the worst straggler so none is lost.
  int min_plant = plant_arc_for(pile_max_lag(s_min, MIN_COUNT, min_target));
  int hour_plant = plant_arc_for(pile_max_lag(s_hour, HOUR_COUNT, hour_target));

  // A and B share the minute hand but keep to opposite halves so they never
  // stand on top of each other: A works the inner half, B the outer half.
  int min_mid = MIN_LEN / 2;

  // Sweeper A always works the inner minute hand. It rests on some stroke slots.
  uint32_t slotA = s_frame / STROKE;
  bool restA = (int)(hash2(0xA1u, slotA) % 100u) < min_rest;
  Stroke A = stroke_state(min_target, MIN_START, min_mid, s_frame, 0, restA,
                          min_plant);
  GPoint a_body = A.body;

  // Sweeper B works the minute hand, then tours to the hour hand and back on a
  // slow cycle, walking through the center (where the hands meet) so it never
  // jumps. w: 0 = on minute, 256 = on hour, in between = walking. Its rest
  // pattern uses a different seed than A, so often only one is working.
  uint32_t Bphase = STROKE / 2 + 211;
  uint32_t slotB = (s_frame + Bphase) / STROKE;
  int bhash = (int)(hash2(0xB2u, slotB) % 100u);
  Stroke Bm = stroke_state(min_target, min_mid, MIN_LEN, s_frame, Bphase,
                           bhash < min_rest, min_plant);
  Stroke Bh = stroke_state(hour_target, HOUR_START, HOUR_LEN, s_frame, Bphase,
                           bhash < hour_rest, hour_plant);

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
  bool b_down, b_moving, b_on_min = false, b_on_hour = false;
  if (w <= 0) {
    b_broom = Bm.broom;
    b_body = Bm.body;
    b_down = Bm.down;
    b_moving = Bm.active;
    b_on_min = true;
  } else if (w >= 256) {
    b_broom = Bh.broom;
    b_body = Bh.body;
    b_down = Bh.down;
    b_moving = Bh.active;
    b_on_hour = true;
  } else {  // walking hand-to-hand: out past the tips and around the rim
    int router = MIN_LEN + 12;
    bool to_hour = cyc < B_MIN_DWELL + B_TRAVEL;
    b_body = travel_point(w, Bm.body, Bh.body, min_target, hour_target, router);
    int wa = w + (to_hour ? 6 : -6);
    if (wa < 0) wa = 0;
    if (wa > 256) wa = 256;
    GPoint ahead =
        travel_point(wa, Bm.body, Bh.body, min_target, hour_target, router);
    int dx = ahead.x - b_body.x, dy = ahead.y - b_body.y;
    int L = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (L < 1) L = 1;
    b_broom = GPoint(b_body.x + dx * 14 / L, b_body.y + dy * 14 / L);
    b_down = false;
    b_moving = true;  // walking
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

  draw_sprite(ctx, b_body, b_broom, b_down, b_moving, s_frame);
  draw_sprite(ctx, a_body, A.broom, A.down, A.active, s_frame);
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
