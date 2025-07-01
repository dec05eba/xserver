/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2025 dec05eba
 */

#include <dix-config.h>

#include "reply.h"
#include "os.h"
#include "dixstruct.h"
#include "misc.h"

#include <stdlib.h>
#include <string.h>
#include <X11/Xproto.h>

static bool
ReplyEnsureCapacity(Reply *reply, size_t newSize)
{
    if (newSize <= reply->capacity)
        return true;

    size_t newCapacity = reply->capacity;
    if (newCapacity == 0)
        newCapacity = 512;

    while (newCapacity < newSize) {
        newCapacity *= 2;
    }

    uint8_t *newBuffer = realloc(reply->buffer, newCapacity);
    if (!newBuffer)
        return false;

    reply->buffer = newBuffer;
    reply->capacity = newCapacity;
    return true;
}

void
ReplyInit(Reply *reply, ClientPtr client)
{
    reply->buffer = NULL;
    reply->size = 0;
    reply->capacity = 0;
    reply->client = client;
}

void
ReplyDeinit(Reply *reply)
{
    if (reply->buffer) {
        free(reply->buffer);
        reply->buffer = NULL;
    }
    reply->size = 0;
    reply->capacity = 0;
    reply->client = NULL;
}

void*
ReplyAppendReplyStruct(Reply *reply, const void *buffer, size_t size, ReplyStruct *replyStruct)
{
    if (size == 0)
        return reply->buffer;

    if (!ReplyEnsureCapacity(reply, reply->size + size))
        return NULL;

    memcpy(reply->buffer + reply->size, buffer, size);
    replyStruct->offset = reply->size;
    reply->size += size;
    return reply->buffer - size;
}

void
ReplyFinalizeReplyStruct(Reply *reply, const ReplyStruct *replyStruct) {
    xGenericReply *genericReply = (xGenericReply*) (reply->buffer + replyStruct->offset);
    genericReply->sequenceNumber = reply->client->sequence;
    genericReply->length = bytes_to_int32(reply->size - replyStruct->offset - sizeof(xGenericReply));
    if (reply->client->swapped) {
        swaps(&genericReply->sequenceNumber);
        swapl(&genericReply->length);
    }
}

bool
ReplyAppendBuffer(Reply *reply, const void *buffer, size_t size)
{
    if (size == 0)
        return true;

    if (!ReplyEnsureCapacity(reply, reply->size + size))
        return false;

    memcpy(reply->buffer + reply->size, buffer, size);
    reply->size += size;
    return true;
}

/* TODO: Make ReplyAppendBuffer always add padding and remove this ReplyAppendBufferWithPadding? */
bool
ReplyAppendBufferWithPadding(Reply *reply, const void *buffer, size_t size)
{
    const size_t paddedSize = pad_to_int32(size);
    if (paddedSize == 0)
        return true;

    if (!ReplyEnsureCapacity(reply, reply->size + paddedSize))
        return false;

    memcpy(reply->buffer + reply->size, buffer, size);
    memset(reply->buffer + reply->size + size, 0, paddedSize - size);
    reply->size += paddedSize;
    return true;
}

bool
ReplyAppendCARD8(Reply *reply, CARD8 value)
{
    if (!ReplyEnsureCapacity(reply, reply->size + sizeof(value)))
        return false;

    *(CARD8*)reply->buffer = value;
    reply->size += sizeof(value);
    return true;
}

bool
ReplyAppendCARD16(Reply *reply, CARD16 value)
{
    if (reply->client->swapped)
        swaps(&value);

    if (!ReplyEnsureCapacity(reply, reply->size + sizeof(value)))
        return false;

    *(CARD16*)reply->buffer = value;
    reply->size += sizeof(value);
    return true;
}

bool
ReplyAppendCARD32(Reply *reply, CARD32 value)
{
    if (reply->client->swapped)
        swaps(&value);

    if (!ReplyEnsureCapacity(reply, reply->size + sizeof(value)))
        return false;

    *(CARD32*)reply->buffer = value;
    reply->size += sizeof(value);
    return true;
}

bool
ReplyAppendCARD32Array(Reply *reply, const CARD32 *values, size_t numValues)
{
    const size_t combinedSize = sizeof(CARD32) * numValues;
    if (combinedSize == 0)
        return true;

    if (!ReplyEnsureCapacity(reply, reply->size + combinedSize))
        return false;

    memcpy(reply->buffer + reply->size, values, combinedSize);
    CARD32 *valuesInBuffer = (CARD32*) (reply->buffer + reply->size);
    if (reply->client->swapped) {
        for (size_t i = 0; i < numValues; ++i) {
            swapl(&valuesInBuffer[i]);
        }
    }

    reply->size += combinedSize;
    return true;
}

/*
 * TODO: Maybe instead of having this have a function that adds padding to next word (CARD32) boundary.
 * This needs to be called to ensure that arrays added align to CARD32 boundary.
 */
bool
ReplyAppendPadding(Reply *reply, size_t paddingSize)
{
    if (paddingSize == 0)
        return true;

    if (!ReplyEnsureCapacity(reply, reply->size + paddingSize))
        return false;

    memset(reply->buffer + reply->size, 0, paddingSize);
    reply->size += paddingSize;
    return true;
}

void
ReplyWriteToClient(Reply *reply)
{
    if (reply->size == 0)
        return;

    /* TODO: Add CARD32 padding at the end if needed? */
    WriteToClient(reply->client, reply->size, reply->buffer);
}