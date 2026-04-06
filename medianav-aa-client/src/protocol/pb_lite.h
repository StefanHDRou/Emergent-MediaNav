/*
 * pb_lite.h / pb_lite.c - Minimal Protobuf Encoder/Decoder
 * (Stubs for AAP standard path - not needed for Companion MJPEG path)
 */

#ifndef MN1AA_PB_LITE_H
#define MN1AA_PB_LITE_H

#include "types.h"

/* Protobuf wire types */
#define PB_WIRE_VARINT      0
#define PB_WIRE_64BIT       1
#define PB_WIRE_LENGTH      2
#define PB_WIRE_32BIT       5

/* Encode a varint into buffer, return bytes written */
int pb_encode_varint(uint8_t* buf, uint64_t value);

/* Encode a field tag (field_number << 3 | wire_type) */
int pb_encode_tag(uint8_t* buf, uint32_t field, uint8_t wireType);

/* Encode a string/bytes field */
int pb_encode_bytes(uint8_t* buf, uint32_t field,
                    const uint8_t* data, uint32_t len);

/* Encode a uint32 field */
int pb_encode_uint32(uint8_t* buf, uint32_t field, uint32_t value);

/* Encode a sint32 field (zigzag encoding) */
int pb_encode_sint32(uint8_t* buf, uint32_t field, int32_t value);

/* Decode a varint from buffer, return bytes consumed */
int pb_decode_varint(const uint8_t* buf, uint32_t maxLen, uint64_t* value);

/* Skip a field in the buffer based on wire type */
int pb_skip_field(const uint8_t* buf, uint32_t maxLen, uint8_t wireType);

#endif /* MN1AA_PB_LITE_H */
