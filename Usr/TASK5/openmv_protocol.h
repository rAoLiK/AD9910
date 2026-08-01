#ifndef OPENMV_PROTOCOL_H
#define OPENMV_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENMV_PROTOCOL_SOF0              (0xAAU)
#define OPENMV_PROTOCOL_SOF1              (0x55U)
#define OPENMV_PROTOCOL_VERSION           (0x02U)
#define OPENMV_PROTOCOL_MAX_PAYLOAD       (64U)
#define OPENMV_PROTOCOL_OVERHEAD          (9U)
#define OPENMV_PROTOCOL_MAX_FRAME_SIZE    \
  (OPENMV_PROTOCOL_MAX_PAYLOAD + OPENMV_PROTOCOL_OVERHEAD)

typedef enum {
  OPENMV_MSG_ACK = 0x01,
  OPENMV_MSG_NACK = 0x02,
  OPENMV_MSG_HEARTBEAT = 0x03,
  OPENMV_MSG_START_TASK = 0x10,
  OPENMV_MSG_COARSE_RESULT = 0x11,
  OPENMV_MSG_DDS_TEST = 0x20,
  OPENMV_MSG_DDS_TEST_RESULT = 0x21,
  OPENMV_MSG_EXIT_TASK = 0x22,
  OPENMV_MSG_VISUAL_LOCK_START = 0x30,
  OPENMV_MSG_VISUAL_LOCK_SAMPLE = 0x31,
  OPENMV_MSG_LOCK_HOLD = 0x32,
  OPENMV_MSG_OPENMV_ERROR = 0x7F
} openmv_message_type_t;

typedef enum {
  OPENMV_NACK_BAD_LENGTH = 0x01,
  OPENMV_NACK_UNSUPPORTED_VERSION = 0x02,
  OPENMV_NACK_BAD_LOCK_MODE = 0x03,
  OPENMV_NACK_BAD_SAW_FREQUENCY = 0x04,
  OPENMV_NACK_BUSY = 0x05,
  OPENMV_NACK_BAD_STATE = 0x06,
  OPENMV_NACK_BAD_SESSION = 0x07,
  OPENMV_NACK_OUT_OF_RANGE = 0x08,
  OPENMV_NACK_IMAGE_ERROR = 0x09,
  OPENMV_NACK_INTERNAL_ERROR = 0x0A,
  OPENMV_NACK_LOCK_HELD = 0x0B
} openmv_nack_code_t;

typedef struct {
  uint8_t version;
  uint8_t type;
  uint8_t seq;
  uint16_t length;
  uint8_t payload[OPENMV_PROTOCOL_MAX_PAYLOAD];
} openmv_frame_t;

typedef struct {
  uint32_t valid_frame_count;
  uint32_t crc_error_count;
  uint32_t length_error_count;
  uint32_t discarded_byte_count;
} openmv_parser_diagnostics_t;

typedef struct {
  uint8_t state;
  uint8_t metadata_index;
  uint16_t payload_index;
  uint8_t metadata[5];
  uint16_t received_crc;
  openmv_frame_t frame;
  openmv_parser_diagnostics_t diagnostics;
} openmv_parser_t;

void OpenMV_Parser_Init(openmv_parser_t *parser);
bool OpenMV_Parser_Feed(openmv_parser_t *parser,
                        uint8_t byte,
                        openmv_frame_t *frame);
void OpenMV_Parser_GetDiagnostics(
    const openmv_parser_t *parser,
    openmv_parser_diagnostics_t *diagnostics);

uint16_t OpenMV_CRC16_Modbus(const uint8_t *data, size_t length);
bool OpenMV_EncodeFrame(uint8_t type,
                        uint8_t seq,
                        const uint8_t *payload,
                        uint16_t payload_length,
                        uint8_t *frame,
                        size_t frame_capacity,
                        size_t *frame_length);

uint16_t OpenMV_ReadU16LE(const uint8_t *data);
uint32_t OpenMV_ReadU32LE(const uint8_t *data);
void OpenMV_WriteU16LE(uint8_t *data, uint16_t value);
void OpenMV_WriteU32LE(uint8_t *data, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* OPENMV_PROTOCOL_H */
