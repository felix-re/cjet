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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "base64.h"
#include "compiler.h"

static const char encode_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64_encode_string(const uint8_t *__restrict in, size_t in_len, char *__restrict out)
{
	unsigned int triple[3];
	unsigned int len;

	while (in_len) {
		len = 0;
		for (unsigned int i = 0; i < 3; i++) {
			if (in_len) {
				triple[i] = *in++;
				len++;
				in_len--;
			} else {
				triple[i] = 0;
			}
		}

		char tmp[4];
		tmp[0] = encode_table[triple[0] >> 2];
		tmp[1] = encode_table[((triple[0] & 0x03) << 4) | ((triple[1] & 0xf0) >> 4)];
		if (likely(len > 1)) {
			tmp[2] = encode_table[((triple[1] & 0x0f) << 2) | ((triple[2] & 0xc0) >> 6)];
		} else {
			tmp[2] = '=';
		}
		if (likely(len > 2)) {
			tmp[3] = encode_table[triple[2] & 0x3f];
		} else {
			tmp[3] = '=';
		}
		memcpy(out, tmp, sizeof(tmp));
		out += 4;
	}

	*out = '\0';
}
