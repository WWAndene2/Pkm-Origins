/* y2r:u: the YUV to RGB converter (the camera module's), as Azahar's (service/cam/y2r_u.cpp and the conversion,
 * hw/y2r.cpp): the conversion's settings set and read back, the conversion itself done at StartConversion in strips
 * of 8 lines (received by the transfer units and gaps of each plane, converted with the fixed-point coefficients,
 * each strip's tiles rotated, written linear or in the PICA's 8 x 8 tiles, sent in the output format), and the
 * transfer-end event signalled 50,000 cycles later (Azahar's MinY2RDelay: the game sees the converter busy first).
 * ORAS opens it after its opening (the movies); with no y2r:u its main thread stopped there (fault 12).
 *
 * As Azahar: the dithering settings kept and returned but not applied, the IsFinished* queries always 1, the
 * addresses the game's own (no other process). */
#include <string.h>

#include "../Ipc.h"
#include "../Memory.h"

enum { MODULE_CAM = 53, SUMMARY_INVALID_ARGUMENT = 7, LEVEL_USAGE = 28, OUT_OF_RANGE = 1021,
       INVALID_ENUM_VALUE = 1005 };

typedef struct { uint32_t address, image_size; uint16_t transfer_unit, gap; } Buffer;

static struct {
    uint8_t input_format, output_format, rotation, block_alignment;
    uint16_t input_line_width, input_lines;
    int16_t coefficients[8];
    uint8_t padding;
    uint16_t alpha;
    Buffer y, u, v, yuyv, dst;
} cvt;

static int spacial_dithering, temporal_dithering, transfer_end_interrupt, busy;
static uint16_t dithering_weights[16];
static HosObject* completion_object;
static int64_t completion_due;

/* the transfer-end event (Azahar makes it with the service: one-shot, "Y2R:Completed") */
static HosObject* completion(void)
{
    if (!completion_object) {
        completion_object = hos_object_new(HOS_EVENT);
        completion_object->reset = HOS_RESET_ONESHOT;
        strcpy(completion_object->name, "Y2R:Completed");
    }
    return completion_object;
}

static const int16_t standard[4][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B}, /* ITU Rec. 601 */
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},  /* ITU Rec. 709 */
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B}, /* ITU Rec. 601, TV ranges */
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421},  /* ITU Rec. 709, TV ranges */
};

static uint32_t error(uint32_t description)
{
    return HOS_RESULT(description, MODULE_CAM, SUMMARY_INVALID_ARGUMENT, LEVEL_USAGE);
}

static uint32_t set_line_width(uint32_t w)
{
    if (w == 0 || w > 1024 || w % 8)
        return error(OUT_OF_RANGE); /* 0xE0E053FD */
    cvt.input_line_width = (uint16_t)w;
    return HOS_OK;
}

static uint32_t set_lines(uint32_t n)
{
    if (n == 0 || n > 1024)
        return error(OUT_OF_RANGE);
    if (n != 1024) /* (the camera module's bug, as Azahar: 1024 leaves the last value) */
        cvt.input_lines = (uint16_t)n;
    return HOS_OK;
}

static uint32_t set_standard(uint32_t k)
{
    if (k >= 4)
        return error(INVALID_ENUM_VALUE); /* 0xE0E053ED */
    memcpy(cvt.coefficients, standard[k], sizeof cvt.coefficients);
    return HOS_OK;
}

static void result(HosRequest* q, uint32_t r)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, r);
}

static void value(HosRequest* q, uint32_t v)
{
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, v);
}

/* ---- the conversion (hw/y2r.cpp) ---- */

typedef uint32_t Tile[64];

/* a range of the game's memory (the whole 4 GB space is reserved: any address is one; the plane addresses are the
   game's own buffers) */
static uint8_t* at(uint32_t a, uint32_t n) { return (uint64_t)a + n <= 0x100000000ull ? hos_pointer(a) : NULL; }

/* a plane's data: n bytes out, from every k-th byte of the transfer units (16-bit planes: their low bytes) */
static int receive(uint8_t* out, Buffer* b, uint32_t amount, int k)
{
    const uint32_t unit = b->transfer_unit / (uint32_t)k;
    if (!unit || amount % unit)
        return -1;
    while (amount) {
        const uint8_t* in = at(b->address, b->transfer_unit);
        if (!in)
            return -1;
        for (uint32_t i = 0; i < unit; i++)
            out[i] = in[i * (uint32_t)k];
        out += unit;
        b->address += b->transfer_unit + b->gap;
        b->image_size -= b->transfer_unit;
        amount -= unit;
    }
    return 0;
}

static int32_t clamp255(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void to_rgb(int format, const uint8_t* y_, const uint8_t* u_, const uint8_t* v_, Tile* tiles, uint32_t width,
                   uint32_t height)
{
    const int16_t* c = cvt.coefficients;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            int32_t Y, U, V;
            if (format == 0 || format == 2) { /* 4:2:2 planes */
                Y = y_[y * width + x];
                U = u_[(y * width + x) / 2];
                V = v_[(y * width + x) / 2];
            } else if (format == 1 || format == 3) { /* 4:2:0 planes */
                Y = y_[y * width + x];
                U = u_[((y / 2) * width + x) / 2];
                V = v_[((y / 2) * width + x) / 2];
            } else { /* YUYV interleaved */
                Y = y_[(y * width + x) * 2];
                U = y_[(y * width + (x / 2) * 2) * 2 + 1];
                V = y_[(y * width + (x / 2) * 2) * 2 + 3];
            }
            const int32_t cy = c[0] * Y;
            int32_t r = cy + c[1] * V, g = cy - c[2] * V - c[3] * U, b = cy + c[4] * U;
            r = (r >> 3) + c[5] + 0x18;
            g = (g >> 3) + c[6] + 0x18;
            b = (b >> 3) + c[7] + 0x18;
            tiles[x / 8][y * 8 + x % 8] = (uint32_t)clamp255(r >> 5) << 24 | (uint32_t)clamp255(g >> 5) << 16 |
                                          (uint32_t)clamp255(b >> 5) << 8;
        }
}

static const uint8_t linear_lut[64] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,
    44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63};
static const uint8_t morton_lut[64] = {
    0,  1,  4,  5,  16, 17, 20, 21, 2,  3,  6,  7,  18, 19, 22, 23, 8,  9,  12, 13, 24, 25,
    28, 29, 10, 11, 14, 15, 26, 27, 30, 31, 32, 33, 36, 37, 48, 49, 52, 53, 34, 35, 38, 39,
    50, 51, 54, 55, 40, 41, 44, 45, 56, 57, 60, 61, 42, 43, 46, 47, 58, 59, 62, 63};

static void rotate(int rotation, const Tile in, Tile out, int h, const uint8_t* map)
{
    int k = 0;
    switch (rotation) {
    case 0: for (int i = 0; i < h * 8; i++) out[map[i]] = in[i]; break;
    case 1: for (int x = 0; x < 8; x++) for (int y = h - 1; y >= 0; y--) out[map[k++]] = in[y * 8 + x]; break;
    case 2: for (int i = h * 8 - 1; i >= 0; i--) out[map[k++]] = in[i]; break;
    default: for (int x = 7; x >= 0; x--) for (int y = 0; y < h; y++) out[map[k++]] = in[y * 8 + x]; break;
    }
}

static void write_tile(uint32_t* out, const Tile t, int h, int stride)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < 8; x++)
            out[y * stride + x] = t[y * 8 + x];
}

static int send(const uint32_t* in, Buffer* b, int amount)
{
    static const int size[4] = {4, 3, 2, 2};
    const int n = size[cvt.output_format & 3];
    while (amount > 0) {
        uint8_t* o = at(b->address, b->transfer_unit);
        if (!o)
            return -1;
        for (uint8_t* end = o + b->transfer_unit; o < end && amount > 0; o += n, amount--) {
            const uint32_t col = *in++;
            const uint8_t r = (uint8_t)(col >> 24), g = (uint8_t)(col >> 16), bl = (uint8_t)(col >> 8),
                          a = (uint8_t)cvt.alpha;
            switch (cvt.output_format & 3) { /* (common/color.h's Encode*: little-endian words) */
            case 0: o[0] = a; o[1] = bl; o[2] = g; o[3] = r; break;
            case 1: o[0] = bl; o[1] = g; o[2] = r; break;
            case 2: { const uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (bl >> 3) << 1 | (a >> 7));
                      o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); break; }
            default: { const uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (bl >> 3));
                       o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); break; }
            }
        }
        b->address += b->transfer_unit + b->gap;
        b->image_size -= b->transfer_unit;
    }
    return 0;
}

static void convert(void)
{
    if (cvt.input_line_width % 8 || cvt.input_line_width > 1024)
        return;
    static uint8_t data[1024 * 8 * 4];
    static Tile tiles[128];
    Tile tmp;
    Buffer y = cvt.y, u = cvt.u, v = cvt.v, yuyv = cvt.yuyv, dst = cvt.dst; /* (Azahar converts a copy) */
    const uint32_t w = cvt.input_line_width, n_tiles = w / 8;
    const uint8_t* map = cvt.block_alignment ? morton_lut : linear_lut;
    for (uint32_t line = 0; line < cvt.input_lines; line += 8) {
        const uint32_t h = cvt.input_lines - line < 8 ? cvt.input_lines - line : 8;
        const uint32_t size = h * w;
        uint8_t *in_y = data, *in_u = in_y + 8 * w, *in_v = in_u + 8 * w / 2;
        const int f = cvt.input_format;
        int bad = 0;
        if (f == 4)
            bad = receive(in_y, &yuyv, size * 2, 1);
        else {
            const int k = f >= 2 ? 2 : 1, sub = f == 1 || f == 3 ? 4 : 2;
            bad = receive(in_y, &y, size, k) || receive(in_u, &u, size / sub, k) || receive(in_v, &v, size / sub, k);
        }
        if (bad || f > 4)
            return;
        to_rgb(f, in_y, in_u, in_v, tiles, w, h);
        uint32_t* out = (uint32_t*)data;
        for (uint32_t i = 0; i < n_tiles; i++) {
            const int r = cvt.rotation & 3;
            rotate(r, tiles[r >= 2 ? n_tiles - i - 1 : i], tmp, (int)h, map);
            const int strip = r & 1 ? 8 : (int)w, stride = r & 1 ? 8 * (int)h : 8;
            if (cvt.block_alignment) {
                write_tile(out, tmp, 8, 8);
                out += 64;
            } else {
                write_tile(out, tmp, (int)h, strip);
                out += stride;
            }
        }
        if (send((const uint32_t*)data, &dst, (int)size))
            return;
    }
}

/* the transfer-end event, 50,000 cycles (186 us) after the start, by the clock thread */
static void completion_tick(void)
{
    if (busy && hos_now_ns() >= completion_due) {
        busy = 0;
        completion()->signaled = 1;
        hos_object_wake(completion());
    }
}

/* ---- the commands ---- */

#define SETTER(name, field)                                                                                             \
    static void name(HosRequest* q) { field = (uint8_t)hos_in(q, 1); result(q, HOS_OK); }
#define GETTER(name, field) static void name(HosRequest* q) { value(q, field); }
SETTER(set_input_format, cvt.input_format)
GETTER(get_input_format, cvt.input_format)
SETTER(set_output_format, cvt.output_format)
GETTER(get_output_format, cvt.output_format)
SETTER(set_rotation, cvt.rotation)
GETTER(get_rotation, cvt.rotation)
SETTER(set_block_alignment, cvt.block_alignment)
GETTER(get_block_alignment, cvt.block_alignment)
static void set_spacial_dithering(HosRequest* q) { spacial_dithering = hos_in(q, 1) & 0xFF; result(q, HOS_OK); }
GETTER(get_spacial_dithering, (uint32_t)spacial_dithering)
static void set_temporal_dithering(HosRequest* q) { temporal_dithering = hos_in(q, 1) & 0xFF; result(q, HOS_OK); }
GETTER(get_temporal_dithering, (uint32_t)temporal_dithering)
static void set_transfer_end_interrupt(HosRequest* q) { transfer_end_interrupt = hos_in(q, 1) & 0xFF; result(q, HOS_OK); }
GETTER(get_transfer_end_interrupt, (uint32_t)transfer_end_interrupt)

static void get_transfer_end_event(HosRequest* q)
{
    uint32_t h = hos_handle_new(completion());
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 0, &h, 1);
}

static void set_sending(HosRequest* q, Buffer* b)
{
    b->address = hos_in(q, 1);
    b->image_size = hos_in(q, 2);
    b->transfer_unit = (uint16_t)hos_in(q, 3);
    b->gap = (uint16_t)hos_in(q, 4);
    result(q, HOS_OK);
}
static void set_sending_y(HosRequest* q) { set_sending(q, &cvt.y); }
static void set_sending_u(HosRequest* q) { set_sending(q, &cvt.u); }
static void set_sending_v(HosRequest* q) { set_sending(q, &cvt.v); }
static void set_sending_yuyv(HosRequest* q) { set_sending(q, &cvt.yuyv); }
static void set_receiving(HosRequest* q) { set_sending(q, &cvt.dst); }
static void finished(HosRequest* q) { value(q, 1); }

static void set_input_line_width(HosRequest* q) { result(q, set_line_width(hos_in(q, 1))); }
GETTER(get_input_line_width, cvt.input_line_width)
static void set_input_lines(HosRequest* q) { result(q, set_lines(hos_in(q, 1))); }
GETTER(get_input_lines, cvt.input_lines)

static void set_coefficient(HosRequest* q)
{
    for (int k = 0; k < 4; k++) {
        const uint32_t w = hos_in(q, 1 + k);
        cvt.coefficients[2 * k] = (int16_t)w;
        cvt.coefficients[2 * k + 1] = (int16_t)(w >> 16);
    }
    result(q, HOS_OK);
}

static void put_coefficients(HosRequest* q, const int16_t* c)
{
    hos_reply(q, 5, 0);
    hos_out(q, 1, HOS_OK);
    for (int k = 0; k < 4; k++)
        hos_out(q, 2 + k, (uint16_t)c[2 * k] | (uint32_t)(uint16_t)c[2 * k + 1] << 16);
}
static void get_coefficient(HosRequest* q) { put_coefficients(q, cvt.coefficients); }
static void set_standard_coefficient(HosRequest* q) { result(q, set_standard(hos_in(q, 1))); }
static void get_standard_coefficient(HosRequest* q)
{
    const uint32_t k = hos_in(q, 1);
    if (k < 4)
        put_coefficients(q, standard[k]);
    else
        result(q, error(INVALID_ENUM_VALUE));
}

static void set_alpha(HosRequest* q) { cvt.alpha = (uint16_t)hos_in(q, 1); result(q, HOS_OK); }
GETTER(get_alpha, cvt.alpha)

static void set_dithering_weight_params(HosRequest* q)
{
    for (int k = 0; k < 8; k++) {
        const uint32_t w = hos_in(q, 1 + k);
        dithering_weights[2 * k] = (uint16_t)w;
        dithering_weights[2 * k + 1] = (uint16_t)(w >> 16);
    }
    result(q, HOS_OK);
}
static void get_dithering_weight_params(HosRequest* q)
{
    hos_reply(q, 9, 0);
    hos_out(q, 1, HOS_OK);
    for (int k = 0; k < 8; k++)
        hos_out(q, 2 + k, dithering_weights[2 * k] | (uint32_t)dithering_weights[2 * k + 1] << 16);
}

static void start_conversion(HosRequest* q)
{
    static int ticking;
    if (!ticking) {
        ticking = 1;
        hos_clock_every(100000, completion_tick); /* (0.1 ms: the event is due 0.19 ms after the start) */
    }
    convert();
    busy = 1;
    completion_due = hos_now_ns() + 50000LL * 1000000000 / 268111856;
    result(q, HOS_OK);
}

static void stop_conversion(HosRequest* q) { busy = 0; result(q, HOS_OK); }
GETTER(is_busy_conversion, (uint32_t)busy)

static void set_package_parameter(HosRequest* q)
{
    const uint32_t w0 = hos_in(q, 1), w1 = hos_in(q, 2), w2 = hos_in(q, 3);
    cvt.input_format = (uint8_t)w0;
    cvt.output_format = (uint8_t)(w0 >> 8);
    cvt.rotation = (uint8_t)(w0 >> 16);
    cvt.block_alignment = (uint8_t)(w0 >> 24);
    uint32_t r = set_line_width(w1 & 0xFFFF);
    if (r == HOS_OK)
        r = set_lines(w1 >> 16);
    if (r == HOS_OK)
        r = set_standard(w2 & 0xFF);
    if (r == HOS_OK) {
        cvt.padding = (uint8_t)(w2 >> 8);
        cvt.alpha = (uint16_t)(w2 >> 16);
    }
    result(q, r);
}

static void ping_process(HosRequest* q) { value(q, 0); }

static void driver_initialize(HosRequest* q)
{
    cvt.input_format = 0;
    cvt.output_format = 0;
    cvt.rotation = 0;
    cvt.block_alignment = 0;
    memset(cvt.coefficients, 0, sizeof cvt.coefficients);
    cvt.input_line_width = 1024;
    cvt.input_lines = 1024;
    cvt.alpha = 0;
    memset(&cvt.y, 0, sizeof cvt.y);
    memset(&cvt.u, 0, sizeof cvt.u);
    memset(&cvt.v, 0, sizeof cvt.v);
    memset(&cvt.dst, 0, sizeof cvt.dst);
    completion()->signaled = 0;
    result(q, HOS_OK);
}

static void driver_finalize(HosRequest* q) { result(q, HOS_OK); }

static void get_package_parameter(HosRequest* q)
{
    hos_reply(q, 4, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, cvt.input_format | (uint32_t)cvt.output_format << 8 | (uint32_t)cvt.rotation << 16 |
                      (uint32_t)cvt.block_alignment << 24);
    hos_out(q, 3, cvt.input_line_width | (uint32_t)cvt.input_lines << 16);
    hos_out(q, 4, (uint16_t)cvt.coefficients[0] | (uint32_t)(uint16_t)cvt.coefficients[1] << 16);
}


static const HosCommandEntry commands[] = {
    {0x0001, set_input_format, "SetInputFormat"},
    {0x0002, get_input_format, "GetInputFormat"},
    {0x0003, set_output_format, "SetOutputFormat"},
    {0x0004, get_output_format, "GetOutputFormat"},
    {0x0005, set_rotation, "SetRotation"},
    {0x0006, get_rotation, "GetRotation"},
    {0x0007, set_block_alignment, "SetBlockAlignment"},
    {0x0008, get_block_alignment, "GetBlockAlignment"},
    {0x0009, set_spacial_dithering, "SetSpacialDithering"},
    {0x000A, get_spacial_dithering, "GetSpacialDithering"},
    {0x000B, set_temporal_dithering, "SetTemporalDithering"},
    {0x000C, get_temporal_dithering, "GetTemporalDithering"},
    {0x000D, set_transfer_end_interrupt, "SetTransferEndInterrupt"},
    {0x000E, get_transfer_end_interrupt, "GetTransferEndInterrupt"},
    {0x000F, get_transfer_end_event, "GetTransferEndEvent"},
    {0x0010, set_sending_y, "SetSendingY"},
    {0x0011, set_sending_u, "SetSendingU"},
    {0x0012, set_sending_v, "SetSendingV"},
    {0x0013, set_sending_yuyv, "SetSendingYUYV"},
    {0x0014, finished, "IsFinishedSendingYuv"},
    {0x0015, finished, "IsFinishedSendingY"},
    {0x0016, finished, "IsFinishedSendingU"},
    {0x0017, finished, "IsFinishedSendingV"},
    {0x0018, set_receiving, "SetReceiving"},
    {0x0019, finished, "IsFinishedReceiving"},
    {0x001A, set_input_line_width, "SetInputLineWidth"},
    {0x001B, get_input_line_width, "GetInputLineWidth"},
    {0x001C, set_input_lines, "SetInputLines"},
    {0x001D, get_input_lines, "GetInputLines"},
    {0x001E, set_coefficient, "SetCoefficient"},
    {0x001F, get_coefficient, "GetCoefficient"},
    {0x0020, set_standard_coefficient, "SetStandardCoefficient"},
    {0x0021, get_standard_coefficient, "GetStandardCoefficient"},
    {0x0022, set_alpha, "SetAlpha"},
    {0x0023, get_alpha, "GetAlpha"},
    {0x0024, set_dithering_weight_params, "SetDitheringWeightParams"},
    {0x0025, get_dithering_weight_params, "GetDitheringWeightParams"},
    {0x0026, start_conversion, "StartConversion"},
    {0x0027, stop_conversion, "StopConversion"},
    {0x0028, is_busy_conversion, "IsBusyConversion"},
    {0x0029, set_package_parameter, "SetPackageParameter"},
    {0x002A, ping_process, "PingProcess"},
    {0x002B, driver_initialize, "DriverInitialize"},
    {0x002C, driver_finalize, "DriverFinalize"},
    {0x002D, get_package_parameter, "GetPackageParameter"},
    {0, NULL, NULL},
};

const HosService hos_y2r_u = {"y2r:u", commands};

