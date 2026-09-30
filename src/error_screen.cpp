#include "error_screen.h"

#include <Arduino.h>
#include <ArduinoLog.h>

/* The ESP32 ROM carries miniz's inflater, as used for the font glyphs. */
#include "miniz.h"

#include "el133.h"
#include "error_image.h"
#include "text_box.h"

/* As with the status line, only this one size is linked. */
#include "roboto24.h"
static const GFXfont &FONT = Roboto24;

static const int PAD_X = 24;
static const int PAD_Y = 16;

/* Push inflated bytes to the panel, switching controllers at the half-way
 * point. Returns the new stream position; bytes past the end of a frame are
 * dropped but still counted, so the caller's length check sees them. */
static size_t push(const uint8_t *data, size_t len, size_t pos)
{
  while (len > 0 && pos < EL133_STREAM_BYTES)
  {
    if (pos == EL133_HALF_BYTES)
    {
      el133_stream_end();
      el133_stream_begin(1);
    }

    const size_t boundary = pos < EL133_HALF_BYTES ? EL133_HALF_BYTES : EL133_STREAM_BYTES;
    const size_t n = min(len, boundary - pos);
    el133_stream_write(data, n);
    data += n;
    len -= n;
    pos += n;
  }
  return pos + len;
}

/* Inflate the built-in frame into both controllers. The output goes through
 * the inflater's 32 KB dictionary, used as a ring, so this needs ~43 KB of
 * heap rather than the whole frame. */
static bool stream_error_image()
{
  /* Both too big for the loop task's stack. */
  tinfl_decompressor *inflator = (tinfl_decompressor *)malloc(sizeof(*inflator));
  uint8_t *dict = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE);
  if (inflator == nullptr || dict == nullptr)
  {
    Log.error("No memory to unpack the error image" CR);
    free(inflator);
    free(dict);
    return false;
  }

  tinfl_init(inflator);
  const uint8_t *in = ERROR_IMAGE_ZLIB;
  size_t in_left = sizeof(ERROR_IMAGE_ZLIB);
  size_t dict_pos = 0;
  size_t pos = 0;
  tinfl_status status;

  el133_stream_begin(0);
  do
  {
    size_t in_bytes = in_left;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_pos;
    status = tinfl_decompress(inflator, in, &in_bytes, dict, dict + dict_pos, &out_bytes,
                              TINFL_FLAG_PARSE_ZLIB_HEADER);
    in += in_bytes;
    in_left -= in_bytes;

    pos = push(dict + dict_pos, out_bytes, pos);
    dict_pos = (dict_pos + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
  } while (status == TINFL_STATUS_HAS_MORE_OUTPUT);
  el133_stream_end();

  free(inflator);
  free(dict);

  if (status != TINFL_STATUS_DONE || pos != EL133_STREAM_BYTES)
  {
    Log.error("Error image did not unpack: status %d, %u of %u bytes" CR, (int)status,
              (unsigned)pos, (unsigned)EL133_STREAM_BYTES);
    return false;
  }
  return true;
}

bool error_screen_show(const char *message)
{
  Log.notice("Showing the error screen" CR);
  el133_init_panel();

  if (message != nullptr && message[0] != '\0')
    text_box_begin(&FONT, PAD_X, PAD_Y, message);

  const bool streamed = stream_error_image();
  text_box_end();

  /* Nothing reaches the glass until the refresh, so a bad unpack leaves the
   * previous image alone. */
  if (!streamed)
    return false;
  return el133_refresh();
}
