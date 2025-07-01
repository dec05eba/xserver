/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2025 dec05eba
 */

/*
 * These Reply methods provide a simple way to send data to the client as a reply.
 * The reply length is automatically calculated and added when |ReplyFinalizeReplyStruct| is called
 * and byteswapped to the client if needed. The |xGenericReply| section of the reply struct is automatically byteswapped
 * (the sequence and length).
 * Example usage (without error checking for simplicity):
 *   xXF86VidModeGetMonitorReply rep = ...
 *   Reply reply; 
 *   ReplyInit(&reply, client);
 *   ReplyStruct replyStruct = {0};
 *   ReplyAppendReplyStruct(&reply, &rep, sizeof(rep), &replyStruct);
 *   ReplyAppendCARD32(&reply, 32);
 *   ReplyAppendCARD32(&reply, 64);
 *   ReplyStructFinalize(&reply, &replyStruct);
 *   ReplyWriteToClient(&reply);
 *   ReplyDeinit(&reply);
 */

#ifndef REPLY_H
#define REPLY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <X11/Xdefs.h>
#include <X11/Xmd.h>

typedef struct {
    size_t offset;
} ReplyStruct;

typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
    ClientPtr client;
} Reply;

void ReplyInit(Reply *reply, ClientPtr client);
void ReplyDeinit(Reply *reply);

/*
 * Returns a pointer to the data inside the buffer.
 * The returned pointer can become invalid if data is append to the reply afterwards.
 */
void* ReplyAppendReplyStruct(Reply *reply, const void *buffer, size_t size, ReplyStruct *replyStruct);
void ReplyFinalizeReplyStruct(Reply *reply, const ReplyStruct *replyStruct);
bool ReplyAppendBuffer(Reply *reply, const void *buffer, size_t size);
bool ReplyAppendBufferWithPadding(Reply *reply, const void *buffer, size_t size);
bool ReplyAppendCARD8(Reply *reply, CARD8 value);
/* Automatically byteswaps if needed by the client */
bool ReplyAppendCARD16(Reply *reply, CARD16 value);
/* Automatically byteswaps if needed by the client */
bool ReplyAppendCARD32(Reply *reply, CARD32 value);
/* Automatically byteswaps if needed by the client */
bool ReplyAppendCARD32Array(Reply *reply, const CARD32 *values, size_t numValues);
bool ReplyAppendPadding(Reply *reply, size_t paddingSize);
void ReplyWriteToClient(Reply *reply);

#endif /* REPLY_H */
