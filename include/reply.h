/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2025 dec05eba
 */

/*
 * These Reply methods provide a simple way to send data to the client as a reply.
 * The reply length is automatically calculated and added at the end when |ReplyWriteToClient| is called
 * and byteswapped to the client if needed.
 * The |xGenericReply| section of the reply struct is automatically byteswapped (the sequence and length).
 * |ReplyAppendReplyStruct| has to be the first function called when adding data.
 * Example usage (without error checking for simplicity):
 *   xXF86VidModeGetMonitorReply rep = ...
 *   Reply reply; 
 *   ReplyInit(&reply, client);
 *   ReplyAppendReplyStruct(&reply, &rep, sizeof(rep));
 *   ReplyAppendCARD32(&reply, 32);
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
    uint8_t *buffer;
    size_t size;
    size_t capacity;
    ClientPtr client;
    bool hasReplyStruct;
} Reply;

void ReplyInit(Reply *reply, ClientPtr client);
void ReplyDeinit(Reply *reply);

bool ReplyAppendReplyStruct(Reply *reply, const void *buffer, size_t size);
bool ReplyAppendBuffer(Reply *reply, const void *buffer, size_t size);
/* Automatically byteswaps if needed by the client */
bool ReplyAppendCARD32(Reply *reply, CARD32 value);
void ReplyWriteToClient(Reply *reply);

#endif /* REPLY_H */
