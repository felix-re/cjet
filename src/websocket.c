/*
 *The MIT License (MIT)
 *
 * Copyright (c) <2016> <Stephan Gatzka>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "base64.h"
#include "compiler.h"
#include "http_connection.h"
#include "jet_endian.h"
#include "jet_random.h"
#include "jet_string.h"
#include "log.h"
#include "sha1/sha1.h"
#include "utf8_checker.h"
#include "util.h"
#include "websocket.h"

static const uint8_t WS_MASK_SET = 0x80;
static const uint8_t WS_HEADER_FIN = 0x80;
static const unsigned int WS_SMALL_FRAME_SIZE = 125;

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define CRLF "\r\n"

#define WS_CONTINUATION_FRAME 0x0
#define WS_TEXT_FRAME 0x1
#define WS_BINARY_FRAME 0x2
#define WS_CLOSE_FRAME 0x8
#define WS_PING_FRAME 0x9
#define WS_PONG_FRAME 0x0a

static void unmask_payload(uint8_t *buffer, size_t length, uint8_t *mask)
{
	size_t bytewidth = sizeof(uint_fast16_t);
	if (length < 8) bytewidth = 1;
	size_t shift = 1;
	if (bytewidth > 2) shift = 2;
	if (bytewidth > 4) shift = 3;

	size_t pre_length, main_length, post_length;
	void *ptr_alligned;
	uint32_t mask32;
	uint32_t *buffer_alligned32;
	uint64_t mask64;
	uint64_t *buffer_alligned64;

	switch (bytewidth) {
	case 8:
		pre_length = ((size_t) buffer) % bytewidth;
		pre_length = bytewidth - pre_length;
		main_length = (length - pre_length) >> shift;
		post_length = length - pre_length - (main_length << shift);
		ptr_alligned = buffer + pre_length;

		mask32 = 0x0;
		for (unsigned int i = 0; i < 4; i++) {
			mask32 |= (((uint32_t) *(mask + (i + pre_length) % 4)) & 0xFF) << (i * 8);
		}
		mask64 = ((uint64_t) mask32) & 0xFFFFFFFF;
		mask64 |= (mask64 << 32) & 0xFFFFFFFF00000000;
		buffer_alligned64 = ptr_alligned;
		for (size_t i = 0; i < pre_length; i++) {
			buffer[i] ^= (mask[i % 4]);
		}
		for (size_t i = 0; i < main_length; i++) {
			buffer_alligned64[i] ^= mask64;
		}
		for (size_t i = length - post_length; i < length; i++) {
			buffer[i] ^= (mask[i % 4]);
		}
		break;
	case 4:
		pre_length = ((size_t) buffer) % bytewidth;
		pre_length = bytewidth - pre_length;
		main_length = (length - pre_length) >> shift;
		post_length = length - pre_length - (main_length << shift);
		ptr_alligned = buffer + pre_length;

		mask32 = 0x0;
		for (unsigned int i = 0; i < 4; i++) {
			mask32 |= (((uint32_t) *(mask + (i + pre_length) % 4)) & 0xFF) << (i * 8);
		}
		buffer_alligned32 = ptr_alligned;
		for (size_t i = 0; i < pre_length; i++) {
			buffer[i] ^= (mask[i % 4]);
		}
		for (size_t i = 0; i < main_length; i++) {
			buffer_alligned32[i] ^= mask32;
		}
		for (size_t i = length - post_length; i < length; i++) {
			buffer[i] ^= (mask[i % 4]);
		}
		break;
	default:
		for (size_t i = 0; i < length; i++) {
			buffer[i] = buffer[i] ^ (mask[i % 4]);
		}
		break;
	}
}

static bool is_status_code_invalid(uint16_t status_code)
{
	bool ret = true;
	if ((status_code >= WS_CLOSE_NORMAL) && (status_code <= WS_CLOSE_UNSUPPORTED)) ret = false;
	if ((status_code >= WS_CLOSE_UNSUPPORTED_DATA) && (status_code <= WS_CLOSE_INTERNAL_ERROR)) ret = false;
	if ((status_code >= WS_CLOSE_RESERVED_LOWER_BOUND) && (status_code <= WS_CLOSE_RESERVED_UPPER_BOUND)) ret = false;
	return ret;
}

static void handle_error(struct websocket *s, uint16_t status_code)
{
	websocket_close(s, status_code);
	s->on_error(s);
}

static enum websocket_callback_return ws_handle_frame(struct websocket *s, uint8_t *frame, size_t length)
{
	if (unlikely(s->ws_flags.rsv != 0)) {
		log_err("Frame with RSV-bit are not supported");
		handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
		return WS_CLOSED;
	}

	if (unlikely((s->ws_flags.fin == 0) && (s->ws_flags.opcode >= WS_PING_FRAME))) {
		log_err("Control Frames must not be fragmented!");
		handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
		return WS_CLOSED;
	}

	if (s->ws_flags.fin == 0) {
		if (s->ws_flags.opcode !=0) {
			if (unlikely(s->ws_flags.is_fragmented)) {
				log_err("Overwriting Opcode of unfinished fragmentation!");
				handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
				return WS_CLOSED;
			}
			s->ws_flags.is_fragmented = 1;
			s->ws_flags.frag_opcode = s->ws_flags.opcode;
			s->ws_flags.opcode = WS_CONTINUATION_FRAME;
		} else {
			if (unlikely(!(s->ws_flags.is_fragmented))) {
				log_err("No start frame was send!");
				handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
				return WS_CLOSED;
			}
		}
	}

	if (s->ws_flags.is_fragmented) {
		if (unlikely((s->ws_flags.opcode < WS_CLOSE_FRAME) && (s->ws_flags.opcode > 0))) {
			log_err("Opcode during fragmentation must be 0x0!");
			handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
			return WS_CLOSED;
		}
	}

	enum websocket_callback_return ret = WS_OK;
	bool last_frame = false;
	switch (s->ws_flags.opcode) {
	case WS_CONTINUATION_FRAME:
		if (unlikely(s->ws_flags.fin ==1)) {
			last_frame = true;
		}
		switch (s->ws_flags.frag_opcode) {
		case WS_BINARY_FRAME:
			ret = s->binary_frame_received(s, frame, length, last_frame);
			break;
		case WS_TEXT_FRAME:
			ret = s->text_frame_received(s, (char *)frame, length, last_frame);
			if (ret == WS_CLOSED) {
				handle_error(s, WS_CLOSE_UNSUPPORTED_DATA);
				return ret;
			}
			break;
		default:
			log_err("Opcode unknown!");
			handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
			return WS_CLOSED;
			break;
		}
		if (last_frame) {
			s->ws_flags.is_fragmented = 0;
			s->ws_flags.frag_opcode = WS_CONTINUATION_FRAME;
		}
		break;

	case WS_BINARY_FRAME:
		if (likely(s->binary_message_received != NULL)) {
			ret = s->binary_message_received(s, frame, length);
		} else {
			handle_error(s, WS_CLOSE_UNSUPPORTED);
			ret = WS_CLOSED;
		}
		break;

	case WS_TEXT_FRAME:
		if (likely(s->text_message_received != NULL)) {
			ret = s->text_message_received(s, (char *)frame, length);
			if (ret == WS_CLOSED) {
				handle_error(s, WS_CLOSE_UNSUPPORTED_DATA);
			}
		} else {
			handle_error(s, WS_CLOSE_UNSUPPORTED);
			ret = WS_CLOSED;
		}
		break;

	case WS_PING_FRAME: {
		if (unlikely(length > WS_SMALL_FRAME_SIZE)) {
			handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
			ret = WS_CLOSED;
		} else {
			int pong_ret = websocket_send_pong_frame(s, frame, length);
			if (unlikely(pong_ret < 0)) {
				// TODO: maybe call the error callback?
				ret = WS_ERROR;
			} else {
				if (s->ping_received != NULL) {
					ret = s->ping_received(s, frame, length);
				}
			}
		}
		break;
	}

	case WS_PONG_FRAME:
		if (unlikely(length > WS_SMALL_FRAME_SIZE)) {
			handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
			ret = WS_CLOSED;
		} else {
			if (s->pong_received != NULL) {
				ret = s->pong_received(s, frame, length);
			}
		}
		break;

	case WS_CLOSE_FRAME: {
		uint16_t status_code = WS_CLOSE_NORMAL;
		if (length >= 2) {
			memcpy(&status_code, frame, sizeof(status_code));
			status_code = jet_be16toh(status_code);
		}
		if (length > 2) {
			struct cjet_utf8_checker c;
			cjet_init_checker(&c);
			if (!cjet_is_byte_sequence_valid(&c, frame + 2, length - 2, true)) {
				handle_error(s, WS_CLOSE_UNSUPPORTED_DATA);
				return WS_CLOSED;
			}
		}
		if ((length == 1) || (length > WS_SMALL_FRAME_SIZE) || is_status_code_invalid(status_code)) {
			handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
			return WS_CLOSED;
		}
		websocket_close(s, WS_CLOSE_NORMAL);
		if (s->close_received != NULL) {
			s->close_received(s, (enum ws_status_code)status_code);
		}
		ret = WS_CLOSED;
		break;
	}

	default:
		log_err("Unsupported websocket frame with reserved opcode!\n");
		handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
		ret = WS_CLOSED;
		break;
	}

	return ret;
}

static enum bs_read_callback_return ws_get_payload(void *context, uint8_t *buf, size_t len)
{
	struct websocket *s = (struct websocket *)context;

	if (unlikely((len == 0) && (s->length != 0))) {
		/*
		 * Other side closed the socket
		 */
		handle_error(s, WS_CLOSE_GOING_AWAY);
		return BS_CLOSED;
	}

	struct buffered_reader *br = &s->connection->br;
	if (unlikely(s->is_server && (s->ws_flags.mask == 0))) {
		handle_error(s, WS_CLOSE_PROTOCOL_ERROR);
		return BS_CLOSED;
	} else {
		if (s->ws_flags.mask != 0) {
			unmask_payload(buf, len, s->mask);
		}
		enum websocket_callback_return ret = ws_handle_frame(s, buf, len);
		switch (ret) {
		case WS_OK:
			br->read_exactly(br->this_ptr, 1, ws_get_header, s);
			return BS_OK;

		case WS_CLOSED:
			return BS_CLOSED;

		case WS_ERROR:
			handle_error(s, WS_CLOSE_INTERNAL_ERROR);
			return BS_CLOSED;
		}
	}
	return BS_OK;
}

static enum bs_read_callback_return ws_get_mask(void *context, uint8_t *buf, size_t len)
{
	struct websocket *s = (struct websocket *)context;

	if (unlikely(len == 0)) {
		handle_error(s, WS_CLOSE_GOING_AWAY);
		return BS_CLOSED;
	}

	memcpy(s->mask, buf, sizeof(s->mask));
	if (likely(s->length > 0)) {
		struct buffered_reader *br = &s->connection->br;
		br->read_exactly(br->this_ptr, s->length, ws_get_payload, s);
		return BS_OK;
	} else {
		return ws_get_payload(s, NULL, 0);
	}
}

static enum bs_read_callback_return read_mask_or_payload(struct websocket *s)
{
	struct buffered_reader *br = &s->connection->br;
	if (s->ws_flags.mask == 1) {
		br->read_exactly(br->this_ptr, sizeof(s->mask), ws_get_mask, s);
		return BS_OK;
	} else {
		if (likely(s->length > 0)) {
			br->read_exactly(br->this_ptr, s->length, ws_get_payload, s);
			return BS_OK;
		} else {
			return ws_get_payload(s, NULL, 0);
		}
	}
}

static enum bs_read_callback_return ws_get_length16(void *context, uint8_t *buf, size_t len)
{
	struct websocket *s = (struct websocket *)context;

	if (unlikely(len == 0)) {
		handle_error(s, WS_CLOSE_GOING_AWAY);
		return BS_CLOSED;
	}

	uint16_t field;
	memcpy(&field, buf, sizeof(field));
	field = jet_be16toh(field);
	s->length = field;
	return read_mask_or_payload(s);
}

static enum bs_read_callback_return ws_get_length64(void *context, uint8_t *buf, size_t len)
{
	struct websocket *s = (struct websocket *)context;

	if (unlikely(len == 0)) {
		handle_error(s, WS_CLOSE_GOING_AWAY);
		return BS_CLOSED;
	}

	uint64_t field;
	memcpy(&field, buf, sizeof(field));
	field = jet_be64toh(field);
	s->length = field;
	return read_mask_or_payload(s);
}

static enum bs_read_callback_return ws_get_first_length(void *context, uint8_t *buf, size_t len)
{
	struct websocket *s = (struct websocket *)context;

	if (unlikely(len == 0)) {
		handle_error(s, WS_CLOSE_GOING_AWAY);
		return BS_CLOSED;
	}

	uint8_t field;
	memcpy(&field, buf, sizeof(field));
	if ((field & WS_MASK_SET) == WS_MASK_SET) {
		s->ws_flags.mask = 1;
	} else {
		s->ws_flags.mask = 0;
	}

	struct buffered_reader *br = &s->connection->br;
	field = field & ~WS_MASK_SET;
	if (field < 126) {
		s->length = field;
		return read_mask_or_payload(s);
	} else if (field == 126) {
		br->read_exactly(br->this_ptr, 2, ws_get_length16, s);
		return BS_OK;
	} else {
		br->read_exactly(br->this_ptr, 8, ws_get_length64, s);
		return BS_OK;
	}
}

enum bs_read_callback_return ws_get_header(void *context, uint8_t *buf, size_t len)
{
	struct websocket *s = (struct websocket *)context;

	if (unlikely(len == 0)) {
		handle_error(s, WS_CLOSE_GOING_AWAY);
		return BS_CLOSED;
	}

	uint8_t field;
	memcpy(&field, buf, sizeof(field));
	if ((field & WS_HEADER_FIN) == WS_HEADER_FIN) {
		s->ws_flags.fin = 1;
	} else {
		s->ws_flags.fin = 0;
	}

	static const uint8_t RSV_MASK = 0x70;
	uint8_t rsv_field;
	rsv_field = field & RSV_MASK;
	rsv_field = rsv_field >> 4;
	s->ws_flags.rsv = rsv_field;

	static const uint8_t OPCODE_MASK = 0x0f;
	field = field & OPCODE_MASK;
	s->ws_flags.opcode = field;
	struct buffered_reader *br = &s->connection->br;
	br->read_exactly(br->this_ptr, 1, ws_get_first_length, s);
	return BS_OK;
}

enum bs_read_callback_return websocket_read_header_line(void *context, uint8_t *buf, size_t len)
{
	struct websocket *s = (struct websocket *)context;

	if (unlikely(len <= 0)) {
		handle_error(s, WS_CLOSE_GOING_AWAY);
		return BS_CLOSED;
	}

	size_t nparsed = http_parser_execute(&s->connection->parser, &s->connection->parser_settings, (const char *)buf, len);
	if (unlikely(nparsed != (size_t)len)) {
		s->connection->status_code = HTTP_BAD_REQUEST;
		send_http_error_response(s->connection);
		handle_error(s, WS_CLOSE_GOING_AWAY);
		return BS_CLOSED;
	}

	struct buffered_reader *br = &s->connection->br;

	if (s->connection->parser.upgrade) {
		s->upgrade_complete = true;
		br->read_exactly(br->this_ptr, 1, ws_get_header, s);
		return BS_OK;
	}

	br->read_until(br->this_ptr, CRLF, websocket_read_header_line, s);
	return BS_OK;
}

static int check_http_version(const struct http_parser *parser)
{
	if (parser->http_major > 1) {
		return 0;
	}
	if ((parser->http_major == 1) && (parser->http_minor >= 1)) {
		return 0;
	} else {
		return -1;
	}
}

static int send_upgrade_response(struct http_connection *connection)
{
	struct websocket *s = connection->parser.data;

	if (s->protocol_requested && !s->sub_protocol.found) {
		return -1;
	}

	char accept_value[28];
	struct SHA1Context context;
	uint8_t sha1_buffer[SHA1HashSize];

	SHA1Reset(&context);
	SHA1Input(&context, s->sec_web_socket_key, SEC_WEB_SOCKET_GUID_LENGTH + SEC_WEB_SOCKET_KEY_LENGTH);
	SHA1Result(&context, sha1_buffer);
	b64_encode_string(sha1_buffer, SHA1HashSize, accept_value);

	static const char switch_response[] =
		"HTTP/1.1 101 Switching Protocols" CRLF
		"Upgrade: websocket" CRLF
		"Connection: Upgrade" CRLF
		"Sec-WebSocket-Accept: ";

	static const char ws_protocol[] =
		CRLF "Sec-Websocket-Protocol: ";

	static const char switch_response_end[] = CRLF CRLF;

	struct socket_io_vector iov[5];
	size_t iov_length = 5;

	iov[0].iov_base = switch_response;
	iov[0].iov_len = sizeof(switch_response) - 1;
	iov[1].iov_base = accept_value;
	iov[1].iov_len = sizeof(accept_value);
	iov[2].iov_base = ws_protocol;
	iov[2].iov_len = sizeof(ws_protocol) - 1;

	if (s->sub_protocol.name != NULL){
		iov[3].iov_base = s->sub_protocol.name;
		iov[3].iov_len = strlen(s->sub_protocol.name);
		 iov[4].iov_base = switch_response_end;
		iov[4].iov_len = sizeof(switch_response_end) - 1;
	} else {
		iov_length = 4;
		iov[3].iov_base = switch_response_end;
		iov[3].iov_len = sizeof(switch_response_end) - 1;
	}

	struct buffered_reader *br = &connection->br;
	return br->writev(br->this_ptr, iov, iov_length);
}

int websocket_upgrade_on_header_field(http_parser *p, const char *at, size_t length)
{
	struct http_connection *connection = container_of(p, struct http_connection, parser);
	struct websocket *s = connection->parser.data;

	static const char sec_key[] = "Sec-WebSocket-Key";
	if ((sizeof(sec_key) - 1 == length) && (jet_strncasecmp(at, sec_key, length) == 0)) {
		s->current_header_field = HEADER_SEC_WEBSOCKET_KEY;
		return 0;
	}

	static const char ws_version[] = "Sec-WebSocket-Version";
	if ((sizeof(ws_version) - 1 == length) && (jet_strncasecmp(at, ws_version, length) == 0)) {
		s->current_header_field = HEADER_SEC_WEBSOCKET_VERSION;
		return 0;
	}

	static const char ws_protocol[] = "Sec-WebSocket-Protocol";
	if ((sizeof(ws_protocol) - 1 == length) && (jet_strncasecmp(at, ws_protocol, length) == 0)) {
		s->current_header_field = HEADER_SEC_WEBSOCKET_PROTOCOL;
		return 0;
	}

	return 0;
}

static int save_websocket_key(uint8_t *dest, const char *at, size_t length)
{
	static const char ws_guid[SEC_WEB_SOCKET_GUID_LENGTH] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

	if (length == SEC_WEB_SOCKET_KEY_LENGTH) {
		memcpy(dest, at, length);
		memcpy(&dest[length], ws_guid, sizeof(ws_guid));
		return 0;
	} else {
		return -1;
	}
}

static int check_websocket_version(const char *at, size_t length)
{
	static const char version[] = "13";
	if ((length == sizeof(version) - 1) && (memcmp(at, version, length) == 0)) {
		return 0;
	} else {
		return -1;
	}
}

static void fill_requested_sub_protocol(struct websocket *s, const char *name, size_t length)
{
	size_t name_length = strlen(s->sub_protocol.name);
	if (name_length == length) {
		if (memcmp(s->sub_protocol.name, name, length) == 0) {
			s->sub_protocol.found = true;
			return;
		}
	}
}

static void check_websocket_protocol(struct websocket *s, const char *at, size_t length)
{
	const char *start = at;
	while (length > 0) {
		if (!isspace(*start) && (*start != ',')) {
			const char *end = start;
			while (length > 0) {
				if (*end == ',') {
					ptrdiff_t len = end - start;
					fill_requested_sub_protocol(s, start, len);
					start = end;
					break;
				}
				end++;
				length--;
			}
			if (length == 0) {
				ptrdiff_t len = end - start;
				fill_requested_sub_protocol(s, start, len);
			}
		} else {
			start++;
			length--;
		}
	}
}

int websocket_upgrade_on_header_value(http_parser *p, const char *at, size_t length)
{
	int ret = 0;

	struct http_connection *connection = container_of(p, struct http_connection, parser);
	struct websocket *s = connection->parser.data;

	switch (s->current_header_field) {
	case HEADER_SEC_WEBSOCKET_KEY:
		ret = save_websocket_key(s->sec_web_socket_key, at, length);
		break;

	case HEADER_SEC_WEBSOCKET_VERSION:
		ret = check_websocket_version(at, length);
		break;

	case HEADER_SEC_WEBSOCKET_PROTOCOL:
		s->protocol_requested = true;
		check_websocket_protocol(s, at, length);
		break;

	case HEADER_UNKNOWN:
	default:
		break;
	}

	s->current_header_field = HEADER_UNKNOWN;
	return ret;
}

int websocket_upgrade_on_headers_complete(http_parser *parser)
{
	if (check_http_version(parser) < 0) {
		return -1;
	}
	if (parser->method != HTTP_GET) {
		return -1;
	}

	struct http_connection *connection = container_of(parser, struct http_connection, parser);
	if (!parser->upgrade) {
		return -1;
	}
	int ret = send_upgrade_response(connection);
	if (ret < 0) {
		return -1;
	} else {
		/*
		 * Returning "1" tells the http parser to skip the body of a message if there is one.
		 */
		return 1;
	}
}

static int send_frame(const struct websocket *s, uint8_t *payload, size_t length, unsigned int type)
{
	char ws_header[14];
	uint8_t first_len;
	size_t header_index = 2;

	ws_header[0] = (uint8_t)(type | WS_HEADER_FIN);
	if (length < 126) {
		first_len = (uint8_t)length;
	} else if (length < 65536) {
		uint16_t be_len = jet_htobe16((uint16_t)length);
		memcpy(&ws_header[2], &be_len, sizeof(be_len));
		header_index += sizeof(be_len);
		first_len = 126;
	} else {
		uint64_t be_len = jet_htobe64((uint64_t)length);
		memcpy(&ws_header[2], &be_len, sizeof(be_len));
		header_index += sizeof(be_len);
		first_len = 127;
	}

	if (s->is_server == false) {
		first_len |= WS_MASK_SET;
		uint8_t mask[4];
		cjet_get_random_bytes(mask, sizeof(mask));
		memcpy(&ws_header[header_index], &mask, sizeof(mask));
		header_index += sizeof(mask);
		unmask_payload(payload, length, mask);
	}
	ws_header[1] = first_len;

	struct socket_io_vector iov[2];
	iov[0].iov_base = ws_header;
	iov[0].iov_len = header_index;
	iov[1].iov_base = payload;
	iov[1].iov_len = length;

	struct buffered_reader *br = &s->connection->br;
	return br->writev(br->this_ptr, iov, ARRAY_SIZE(iov));
}

int websocket_send_binary_frame(const struct websocket *s, uint8_t *payload, size_t length)
{
	return send_frame(s, payload, length, WS_BINARY_FRAME);
}

int websocket_send_text_frame(const struct websocket *s, char *payload, size_t length)
{
	return send_frame(s, (uint8_t *)payload, length, WS_TEXT_FRAME);
}

int websocket_send_ping_frame(const struct websocket *s, uint8_t *payload, size_t length)
{
	return send_frame(s, payload, length, WS_PING_FRAME);
}

int websocket_send_pong_frame(const struct websocket *s, uint8_t *payload, size_t length)
{
	return send_frame(s, payload, length, WS_PONG_FRAME);
}

int websocket_send_close_frame(const struct websocket *s, enum ws_status_code status_code)
{
	uint16_t code = status_code;
	code = jet_htobe16(code);
	return send_frame(s, (uint8_t *)&code, sizeof(code), WS_CLOSE_FRAME);
}

int websocket_init(struct websocket *ws, struct http_connection *connection, bool is_server, void (*on_error)(struct websocket *s), const char *sub_protocol)
{
	if (unlikely(sub_protocol == NULL)) {
        log_info("No sub-protocol specified");
//		return -1;
	}
	if (unlikely(on_error == NULL)) {
		log_err("You must specify an error routine");
		return -1;
	}

	memset(ws, 0, sizeof(*ws));
	ws->on_error = on_error;
	ws->connection = connection;
	ws->current_header_field = HEADER_UNKNOWN;
	ws->is_server = is_server;
	ws->upgrade_complete = false;

	ws->sub_protocol.name = sub_protocol;
	ws->ws_flags.is_fragmented = 0;
	ws->ws_flags.frag_opcode = WS_CONTINUATION_FRAME;
	return 0;
}

void websocket_close(struct websocket *ws, enum ws_status_code status_code)
{
	if (ws->upgrade_complete) {
		websocket_send_close_frame(ws, status_code);
	}

	free_connection(ws->connection);
}
