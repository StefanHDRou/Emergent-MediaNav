/*
 * pb_lite.c - Minimal Protobuf Implementation
 *
 * Just enough protobuf to encode/decode AAP control messages.
 * No memory allocation, no schema compilation - hand-rolled for minimal size.
 */

#include "protocol/pb_lite.h"

int pb_encode_varint(uint8_t* buf, uint64_t value)
{
    int n = 0;
    while (value > 0x7F) {
        buf[n++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    buf[n++] = (uint8_t)(value & 0x7F);
    return n;
}

int pb_encode_tag(uint8_t* buf, uint32_t field, uint8_t wireType)
{
    return pb_encode_varint(buf, ((uint64_t)field << 3) | wireType);
}

int pb_encode_bytes(uint8_t* buf, uint32_t field,
                    const uint8_t* data, uint32_t len)
{
    int n = 0;
    n += pb_encode_tag(buf + n, field, PB_WIRE_LENGTH);
    n += pb_encode_varint(buf + n, len);
    memcpy(buf + n, data, len);
    n += len;
    return n;
}

int pb_encode_uint32(uint8_t* buf, uint32_t field, uint32_t value)
{
    int n = 0;
    n += pb_encode_tag(buf + n, field, PB_WIRE_VARINT);
    n += pb_encode_varint(buf + n, value);
    return n;
}

int pb_encode_sint32(uint8_t* buf, uint32_t field, int32_t value)
{
    /* Zigzag encoding: (value << 1) ^ (value >> 31) */
    uint32_t zigzag = (uint32_t)((value << 1) ^ (value >> 31));
    return pb_encode_uint32(buf, field, zigzag);
}

int pb_decode_varint(const uint8_t* buf, uint32_t maxLen, uint64_t* value)
{
    uint64_t result = 0;
    int shift = 0;
    uint32_t i = 0;

    while (i < maxLen && i < 10) {
        uint8_t byte = buf[i];
        result |= ((uint64_t)(byte & 0x7F)) << shift;
        i++;
        if (!(byte & 0x80)) {
            *value = result;
            return (int)i;
        }
        shift += 7;
    }

    *value = 0;
    return -1;
}

int pb_skip_field(const uint8_t* buf, uint32_t maxLen, uint8_t wireType)
{
    uint64_t len;
    int n;

    switch (wireType) {
        case PB_WIRE_VARINT:
            n = pb_decode_varint(buf, maxLen, &len);
            return n;
        case PB_WIRE_64BIT:
            return 8;
        case PB_WIRE_LENGTH:
            n = pb_decode_varint(buf, maxLen, &len);
            if (n < 0) return -1;
            return n + (int)len;
        case PB_WIRE_32BIT:
            return 4;
        default:
            return -1;
    }
}
