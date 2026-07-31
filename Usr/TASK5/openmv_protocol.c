#include "openmv_protocol.h"

#include <string.h>

enum {
  OPENMV_PARSE_SOF0 = 0,
  OPENMV_PARSE_SOF1,
  OPENMV_PARSE_METADATA,
  OPENMV_PARSE_PAYLOAD,
  OPENMV_PARSE_CRC_LOW,
  OPENMV_PARSE_CRC_HIGH
};

static void OpenMV_Parser_ResetFrame(openmv_parser_t *parser)
{
  parser->state = OPENMV_PARSE_SOF0;
  parser->metadata_index = 0U;
  parser->payload_index = 0U;
  parser->received_crc = 0U;
}

static uint16_t OpenMV_Parser_FrameCRC(const openmv_parser_t *parser)
{
  uint16_t crc = 0xFFFFU;
  uint16_t i;
  uint8_t bit;

  for (i = 0U; i < sizeof(parser->metadata); i++) {
    crc ^= parser->metadata[i];
    for (bit = 0U; bit < 8U; bit++) {
      crc = ((crc & 1U) != 0U)
                ? (uint16_t)((crc >> 1U) ^ 0xA001U)
                : (uint16_t)(crc >> 1U);
    }
  }
  for (i = 0U; i < parser->frame.length; i++) {
    crc ^= parser->frame.payload[i];
    for (bit = 0U; bit < 8U; bit++) {
      crc = ((crc & 1U) != 0U)
                ? (uint16_t)((crc >> 1U) ^ 0xA001U)
                : (uint16_t)(crc >> 1U);
    }
  }
  return crc;
}

void OpenMV_Parser_Init(openmv_parser_t *parser)
{
  if (parser == NULL) {
    return;
  }
  memset(parser, 0, sizeof(*parser));
  OpenMV_Parser_ResetFrame(parser);
}

bool OpenMV_Parser_Feed(openmv_parser_t *parser,
                        uint8_t byte,
                        openmv_frame_t *frame)
{
  uint16_t expected_crc;

  if ((parser == NULL) || (frame == NULL)) {
    return false;
  }

  switch (parser->state) {
    case OPENMV_PARSE_SOF0:
      if (byte == OPENMV_PROTOCOL_SOF0) {
        parser->state = OPENMV_PARSE_SOF1;
      } else {
        parser->diagnostics.discarded_byte_count++;
      }
      break;

    case OPENMV_PARSE_SOF1:
      if (byte == OPENMV_PROTOCOL_SOF1) {
        parser->metadata_index = 0U;
        parser->state = OPENMV_PARSE_METADATA;
      } else if (byte != OPENMV_PROTOCOL_SOF0) {
        parser->diagnostics.discarded_byte_count++;
        parser->state = OPENMV_PARSE_SOF0;
      }
      break;

    case OPENMV_PARSE_METADATA:
      parser->metadata[parser->metadata_index++] = byte;
      if (parser->metadata_index == sizeof(parser->metadata)) {
        parser->frame.version = parser->metadata[0];
        parser->frame.type = parser->metadata[1];
        parser->frame.seq = parser->metadata[2];
        parser->frame.length =
            OpenMV_ReadU16LE(&parser->metadata[3]);
        if (parser->frame.length > OPENMV_PROTOCOL_MAX_PAYLOAD) {
          parser->diagnostics.length_error_count++;
          OpenMV_Parser_ResetFrame(parser);
        } else if (parser->frame.length == 0U) {
          parser->state = OPENMV_PARSE_CRC_LOW;
        } else {
          parser->payload_index = 0U;
          parser->state = OPENMV_PARSE_PAYLOAD;
        }
      }
      break;

    case OPENMV_PARSE_PAYLOAD:
      parser->frame.payload[parser->payload_index++] = byte;
      if (parser->payload_index == parser->frame.length) {
        parser->state = OPENMV_PARSE_CRC_LOW;
      }
      break;

    case OPENMV_PARSE_CRC_LOW:
      parser->received_crc = byte;
      parser->state = OPENMV_PARSE_CRC_HIGH;
      break;

    case OPENMV_PARSE_CRC_HIGH:
      parser->received_crc |= (uint16_t)byte << 8U;
      expected_crc = OpenMV_Parser_FrameCRC(parser);
      if (expected_crc == parser->received_crc) {
        *frame = parser->frame;
        parser->diagnostics.valid_frame_count++;
        OpenMV_Parser_ResetFrame(parser);
        return true;
      }
      parser->diagnostics.crc_error_count++;
      OpenMV_Parser_ResetFrame(parser);
      break;

    default:
      OpenMV_Parser_ResetFrame(parser);
      break;
  }

  return false;
}

void OpenMV_Parser_GetDiagnostics(
    const openmv_parser_t *parser,
    openmv_parser_diagnostics_t *diagnostics)
{
  if ((parser == NULL) || (diagnostics == NULL)) {
    return;
  }
  *diagnostics = parser->diagnostics;
}

uint16_t OpenMV_CRC16_Modbus(const uint8_t *data, size_t length)
{
  uint16_t crc = 0xFFFFU;
  size_t i;
  uint8_t bit;

  if ((data == NULL) && (length != 0U)) {
    return 0U;
  }

  for (i = 0U; i < length; i++) {
    crc ^= data[i];
    for (bit = 0U; bit < 8U; bit++) {
      crc = ((crc & 1U) != 0U)
                ? (uint16_t)((crc >> 1U) ^ 0xA001U)
                : (uint16_t)(crc >> 1U);
    }
  }
  return crc;
}

bool OpenMV_EncodeFrame(uint8_t type,
                        uint8_t seq,
                        const uint8_t *payload,
                        uint16_t payload_length,
                        uint8_t *frame,
                        size_t frame_capacity,
                        size_t *frame_length)
{
  size_t total_length;
  uint16_t crc;

  if ((frame == NULL) || (frame_length == NULL) ||
      (payload_length > OPENMV_PROTOCOL_MAX_PAYLOAD) ||
      ((payload == NULL) && (payload_length != 0U))) {
    return false;
  }

  total_length = (size_t)payload_length + OPENMV_PROTOCOL_OVERHEAD;
  if (frame_capacity < total_length) {
    return false;
  }

  frame[0] = OPENMV_PROTOCOL_SOF0;
  frame[1] = OPENMV_PROTOCOL_SOF1;
  frame[2] = OPENMV_PROTOCOL_VERSION;
  frame[3] = type;
  frame[4] = seq;
  OpenMV_WriteU16LE(&frame[5], payload_length);
  if (payload_length != 0U) {
    memcpy(&frame[7], payload, payload_length);
  }
  crc = OpenMV_CRC16_Modbus(
      &frame[2], (size_t)payload_length + 5U);
  OpenMV_WriteU16LE(&frame[7U + payload_length], crc);
  *frame_length = total_length;
  return true;
}

uint16_t OpenMV_ReadU16LE(const uint8_t *data)
{
  if (data == NULL) {
    return 0U;
  }
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

uint32_t OpenMV_ReadU32LE(const uint8_t *data)
{
  if (data == NULL) {
    return 0UL;
  }
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8U) |
         ((uint32_t)data[2] << 16U) |
         ((uint32_t)data[3] << 24U);
}

void OpenMV_WriteU16LE(uint8_t *data, uint16_t value)
{
  if (data == NULL) {
    return;
  }
  data[0] = (uint8_t)(value & 0xFFU);
  data[1] = (uint8_t)(value >> 8U);
}

void OpenMV_WriteU32LE(uint8_t *data, uint32_t value)
{
  if (data == NULL) {
    return;
  }
  data[0] = (uint8_t)(value & 0xFFU);
  data[1] = (uint8_t)((value >> 8U) & 0xFFU);
  data[2] = (uint8_t)((value >> 16U) & 0xFFU);
  data[3] = (uint8_t)((value >> 24U) & 0xFFU);
}
