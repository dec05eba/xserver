/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2025 dec05eba
 */

#include <dix-config.h>

#include "reply.h"
#include "os.h"
#include "dixstruct.h"
#include "os/bug_priv.h"

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
    reply->hasReplyStruct = false;
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
    reply->hasReplyStruct = false;
}

bool
ReplyAppendReplyStruct(Reply *reply, const void *buffer, size_t size)
{
    BUG_WARN_MSG(reply->hasReplyStruct, "The reply already has reply struct added\n");
    BUG_WARN_MSG(reply->size != 0, "The reply has data added before the reply struct. The reply struct has to be the first data added\n");
    BUG_WARN_MSG(((xGenericReply*)buffer)->type != X_Reply, "The added data is not a reply struct\n");
    if (size == 0)
        return true;

    if (!ReplyEnsureCapacity(reply, reply->size + size))
        return false;

    memcpy(reply->buffer + reply->size, buffer, size);
    reply->size += size;
    return true;
}

bool
ReplyAppendBuffer(Reply *reply, const void *buffer, size_t size)
{
    BUG_WARN_MSG(!reply->hasReplyStruct, "The reply has to have reply struct added before data is appended\n");
    if (size == 0)
        return true;

    if (!ReplyEnsureCapacity(reply, reply->size + size))
        return false;

    memcpy(reply->buffer + reply->size, buffer, size);
    reply->size += size;
    return true;
}

bool
ReplyAppendCARD32(Reply *reply, CARD32 value)
{
    if (reply->client->swapped)
        swapl(&value);
    return ReplyAppendBuffer(reply, &value, sizeof(value));
}

void
ReplyWriteToClient(Reply *reply)
{
    BUG_WARN_MSG(!reply->hasReplyStruct, "The reply has to have reply struct added before data is written to the client\n");
    if (reply->size == 0)
        return;

    xGenericReply *genericReply = (xGenericReply*) reply->buffer;
    genericReply->sequenceNumber = reply->client->sequence;
    genericReply->length = bytes_to_int32(reply->size - sizeof(xGenericReply));

    if (reply->client->swapped) {
        swaps(&genericReply->sequenceNumber);
        swapl(&genericReply->length);
    }

    WriteToClient(reply->client, reply->size, reply->buffer);
}