/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SSH message builder functions.
 *
 * Copyright (C) 2019-2020 Maximilian Luz <luzmaximilian@gmail.com>
 */

#ifndef _SURFACE_AGGREGATOR_SSH_MSGB_H
#define _SURFACE_AGGREGATOR_SSH_MSGB_H

#include <asm/unaligned.h>
#include <linux/types.h>

#include <linux/surface_aggregator/controller.h>
#include <linux/surface_aggregator/serial_hub.h>


/**
 * struct msgbuf - Buffer struct to construct SSH messages.
 * @begin: Pointer to the beginning of the allocated buffer space.
 * @end:   Pointer to the end (one past last element) of the allocated buffer
 *         space.
 * @ptr:   Pointer to the first free element in the buffer.
 */
struct msgbuf {
	u8 *begin;
	u8 *end;
	u8 *ptr;
};

/**
 * msgb_init() - Initialize the given message buffer struct.
 * @msgb: The buffer struct to initialize
 * @ptr:  Pointer to the underlying memory by which the buffer will be backed.
 * @cap:  Size of the underlying memory.
 *
 * Initialize the given message buffer struct using the provided memory as
 * backing.
 */
static inline void msgb_init(struct msgbuf *msgb, u8 *ptr, size_t cap)
{
	msgb->begin = ptr;
	msgb->end = ptr + cap;
	msgb->ptr = ptr;
}

/**
 * msgb_bytes_used() - Return the current number of bytes used in the buffer.
 * @msgb: The message buffer.
 */
static inline size_t msgb_bytes_used(const struct msgbuf *msgb)
{
	return msgb->ptr - msgb->begin;
}

/**
 * msgb_push_u16() - Push a u16 value to the buffer.
 * @msgb:  The message buffer.
 * @value: The value to push to the buffer.
 */
static inline void msgb_push_u16(struct msgbuf *msgb, u16 value)
{
	if (WARN_ON(msgb->ptr + sizeof(u16) > msgb->end))
		return;

	put_unaligned_le16(value, msgb->ptr);
	msgb->ptr += sizeof(u16);
}

/**
 * msgb_push_syn() - Push SSH SYN bytes to the buffer.
 * @msgb: The message buffer.
 */
static inline void msgb_push_syn(struct msgbuf *msgb)
{
	msgb_push_u16(msgb, SSH_MSG_SYN);
}

/**
 * msgb_push_buf() - Push raw data to the buffer.
 * @msgb: The message buffer.
 * @buf:  The data to push to the buffer.
 * @len:  The length of the data to push to the buffer.
 */
static inline void msgb_push_buf(struct msgbuf *msgb, const u8 *buf, size_t len)
{
	msgb->ptr = memcpy(msgb->ptr, buf, len) + len;
}

/**
 * msgb_push_crc() - Compute CRC and push it to the buffer.
 * @msgb: The message buffer.
 * @buf:  The data for which the CRC should be computed.
 * @len:  The length of the data for which the CRC should be computed.
 */
static inline void msgb_push_crc(struct msgbuf *msgb, const u8 *buf, size_t len)
{
	msgb_push_u16(msgb, ssh_crc(buf, len));
}

/**
 * msgb_push_frame() - Push a SSH message frame header to the buffer.
 * @msgb: The message buffer
 * @ty:   The type of the frame.
 * @len:  The length of the payload of the frame.
 * @seq:  The sequence ID of the frame/packet.
 */
static inline void msgb_push_frame(struct msgbuf *msgb, u8 ty, u16 len, u8 seq)
{
	struct ssh_frame *frame = (struct ssh_frame *)msgb->ptr;
	const u8 *const begin = msgb->ptr;

	if (WARN_ON(msgb->ptr + sizeof(*frame) > msgb->end))
		return;

	frame->type = ty;
	put_unaligned_le16(len, &frame->len);
	frame->seq  = seq;

	msgb->ptr += sizeof(*frame);
	msgb_push_crc(msgb, begin, msgb->ptr - begin);
}

/**
 * msgb_push_ack() - Push a SSH ACK frame to the buffer.
 * @msgb: The message buffer
 * @seq:  The sequence ID of the frame/packet to be ACKed.
 */
static inline void msgb_push_ack(struct msgbuf *msgb, u8 seq)
{
	// SYN
	msgb_push_syn(msgb);

	// ACK-type frame + CRC
	msgb_push_frame(msgb, SSH_FRAME_TYPE_ACK, 0x00, seq);

	// payload CRC (ACK-type frames do not have a payload)
	msgb_push_crc(msgb, msgb->ptr, 0);
}

/**
 * msgb_push_nak() - Push a SSH NAK frame to the buffer.
 * @msgb: The message buffer
 */
static inline void msgb_push_nak(struct msgbuf *msgb)
{
	// SYN
	msgb_push_syn(msgb);

	// NAK-type frame + CRC
	msgb_push_frame(msgb, SSH_FRAME_TYPE_NAK, 0x00, 0x00);

	// payload CRC (ACK-type frames do not have a payload)
	msgb_push_crc(msgb, msgb->ptr, 0);
}

/**
 * msgb_push_cmd() - Push a SSH command frame with payload to the buffer.
 * @msgb: The message buffer.
 * @seq:  The sequence ID (SEQ) of the frame/packet.
 * @rqid: The request ID (RQID) of the request contained in the frame.
 * @rqst: The request to wrap in the frame.
 */
static inline void msgb_push_cmd(struct msgbuf *msgb, u8 seq, u16 rqid,
				 const struct ssam_request *rqst)
{
	struct ssh_command *cmd;
	const u8 *cmd_begin;
	const u8 type = SSH_FRAME_TYPE_DATA_SEQ;

	// SYN
	msgb_push_syn(msgb);

	// command frame + crc
	msgb_push_frame(msgb, type, sizeof(*cmd) + rqst->length, seq);

	// frame payload: command struct + payload
	if (WARN_ON(msgb->ptr + sizeof(*cmd) > msgb->end))
		return;

	cmd_begin = msgb->ptr;
	cmd = (struct ssh_command *)msgb->ptr;

	cmd->type    = SSH_PLD_TYPE_CMD;
	cmd->tc      = rqst->target_category;
	cmd->tid_out = rqst->target_id;
	cmd->tid_in  = 0x00;
	cmd->iid     = rqst->instance_id;
	put_unaligned_le16(rqid, &cmd->rqid);
	cmd->cid     = rqst->command_id;

	msgb->ptr += sizeof(*cmd);

	// command payload
	msgb_push_buf(msgb, rqst->payload, rqst->length);

	// crc for command struct + payload
	msgb_push_crc(msgb, cmd_begin, msgb->ptr - cmd_begin);
}

#endif /* _SURFACE_AGGREGATOR_SSH_MSGB_H */
