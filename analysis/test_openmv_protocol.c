#include "openmv_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_reference_start_frame(void)
{
  static const uint8_t payload[] = {
      0x01, 0x00, 0x00, 0xE8, 0x03, 0x00, 0x00};
  static const uint8_t expected[] = {
      0xAA, 0x55, 0x02, 0x10, 0x01, 0x07, 0x00,
      0x01, 0x00, 0x00, 0xE8, 0x03, 0x00, 0x00,
      0x6B, 0x90};
  uint8_t encoded[OPENMV_PROTOCOL_MAX_FRAME_SIZE];
  size_t length = 0U;

  assert(OpenMV_EncodeFrame(
      OPENMV_MSG_START_TASK, 0x01U,
      payload, sizeof(payload),
      encoded, sizeof(encoded), &length));
  assert(length == sizeof(expected));
  assert(memcmp(encoded, expected, sizeof(expected)) == 0);
}

static void test_fragment_noise_and_crc_recovery(void)
{
  static const uint8_t payload[] = {0x10, 0x25, 0x00};
  uint8_t encoded[OPENMV_PROTOCOL_MAX_FRAME_SIZE];
  uint8_t corrupt[OPENMV_PROTOCOL_MAX_FRAME_SIZE];
  size_t length = 0U;
  openmv_parser_t parser;
  openmv_frame_t frame;
  openmv_parser_diagnostics_t diagnostics;
  size_t i;
  bool received = false;

  assert(OpenMV_EncodeFrame(
      OPENMV_MSG_ACK, 0x33U, payload, sizeof(payload),
      encoded, sizeof(encoded), &length));
  memcpy(corrupt, encoded, length);
  corrupt[length - 1U] ^= 0x80U;

  OpenMV_Parser_Init(&parser);
  assert(!OpenMV_Parser_Feed(&parser, 0x12U, &frame));
  assert(!OpenMV_Parser_Feed(&parser, 0xAAU, &frame));
  assert(!OpenMV_Parser_Feed(&parser, 0xAAU, &frame));
  for (i = 0U; i < length; i++) {
    assert(!OpenMV_Parser_Feed(&parser, corrupt[i], &frame));
  }
  for (i = 0U; i < length; i++) {
    if (OpenMV_Parser_Feed(&parser, encoded[i], &frame)) {
      received = true;
    }
  }

  assert(received);
  assert(frame.version == OPENMV_PROTOCOL_VERSION);
  assert(frame.type == OPENMV_MSG_ACK);
  assert(frame.seq == 0x33U);
  assert(frame.length == sizeof(payload));
  assert(memcmp(frame.payload, payload, sizeof(payload)) == 0);

  OpenMV_Parser_GetDiagnostics(&parser, &diagnostics);
  assert(diagnostics.crc_error_count == 1U);
  assert(diagnostics.valid_frame_count == 1U);
  assert(diagnostics.discarded_byte_count >= 1U);
}

static void test_little_endian_helpers(void)
{
  uint8_t data[4];

  OpenMV_WriteU16LE(data, 0xA15CU);
  assert(data[0] == 0x5CU);
  assert(data[1] == 0xA1U);
  assert(OpenMV_ReadU16LE(data) == 0xA15CU);

  OpenMV_WriteU32LE(data, 0x89ABCDEFUL);
  assert(OpenMV_ReadU32LE(data) == 0x89ABCDEFUL);
}

static void test_oversize_and_back_to_back_frames(void)
{
  static const uint8_t invalid_header[] = {
      0xAA, 0x55, 0x02, 0x10, 0x70, 0x41, 0x00};
  static const uint8_t payload[] = {0x20, 0x08, 0x00};
  uint8_t encoded[OPENMV_PROTOCOL_MAX_FRAME_SIZE];
  size_t length = 0U;
  openmv_parser_t parser;
  openmv_frame_t frame;
  openmv_parser_diagnostics_t diagnostics;
  size_t i;
  unsigned int received = 0U;

  assert(OpenMV_EncodeFrame(
      OPENMV_MSG_ACK, 0x12U, payload, sizeof(payload),
      encoded, sizeof(encoded), &length));
  OpenMV_Parser_Init(&parser);

  for (i = 0U; i < sizeof(invalid_header); i++) {
    assert(!OpenMV_Parser_Feed(
        &parser, invalid_header[i], &frame));
  }
  for (i = 0U; i < length; i++) {
    if (OpenMV_Parser_Feed(&parser, encoded[i], &frame)) {
      received++;
    }
  }
  for (i = 0U; i < length; i++) {
    if (OpenMV_Parser_Feed(&parser, encoded[i], &frame)) {
      received++;
    }
  }

  assert(received == 2U);
  OpenMV_Parser_GetDiagnostics(&parser, &diagnostics);
  assert(diagnostics.length_error_count == 1U);
  assert(diagnostics.valid_frame_count == 2U);
}

int main(void)
{
  test_reference_start_frame();
  test_fragment_noise_and_crc_recovery();
  test_little_endian_helpers();
  test_oversize_and_back_to_back_frames();
  puts("openmv protocol tests passed");
  return 0;
}
