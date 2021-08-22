/* $OpenBSD$ */

/*
 * Copyright (c) 2008 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "tmux.h"

struct utf8_item {
	RB_ENTRY(utf8_item)	index_entry;
	u_int			index;

	RB_ENTRY(utf8_item)	data_entry;
	char			data[UTF8_SIZE];
	u_char			size;
};

static int
utf8_data_cmp(struct utf8_item *ui1, struct utf8_item *ui2)
{
	if (ui1->size < ui2->size)
		return (-1);
	if (ui1->size > ui2->size)
		return (1);
	return (memcmp(ui1->data, ui2->data, ui1->size));
}
RB_HEAD(utf8_data_tree, utf8_item);
RB_GENERATE_STATIC(utf8_data_tree, utf8_item, data_entry, utf8_data_cmp);
static struct utf8_data_tree utf8_data_tree = RB_INITIALIZER(utf8_data_tree);

static int
utf8_index_cmp(struct utf8_item *ui1, struct utf8_item *ui2)
{
	if (ui1->index < ui2->index)
		return (-1);
	if (ui1->index > ui2->index)
		return (1);
	return (0);
}
RB_HEAD(utf8_index_tree, utf8_item);
RB_GENERATE_STATIC(utf8_index_tree, utf8_item, index_entry, utf8_index_cmp);
static struct utf8_index_tree utf8_index_tree = RB_INITIALIZER(utf8_index_tree);

static u_int utf8_next_index;

#define UTF8_GET_SIZE(uc) (((uc) >> 24) & 0x1f)
#define UTF8_GET_WIDTH(flags) (((uc) >> 29) - 1)

#define UTF8_SET_SIZE(size) (((utf8_char)(size)) << 24)
#define UTF8_SET_WIDTH(width) ((((utf8_char)(width)) + 1) << 29)

/* Get a UTF-8 item from data. */
static struct utf8_item *
utf8_item_by_data(const char *data, size_t size)
{
	struct utf8_item	ui;

	memcpy(ui.data, data, size);
	ui.size = size;

	return (RB_FIND(utf8_data_tree, &utf8_data_tree, &ui));
}

/* Get a UTF-8 item from data. */
static struct utf8_item *
utf8_item_by_index(u_int index)
{
	struct utf8_item	ui;

	ui.index = index;

	return (RB_FIND(utf8_index_tree, &utf8_index_tree, &ui));
}

/* Add a UTF-8 item. */
static int
utf8_put_item(const char *data, size_t size, u_int *index)
{
	struct utf8_item	*ui;

	ui = utf8_item_by_data(data, size);
	if (ui != NULL) {
		*index = ui->index;
		log_debug("%s: found %.*s = %u", __func__, (int)size, data,
		    *index);
		return (0);
	}

	if (utf8_next_index == 0xffffff + 1)
		return (-1);

	ui = xcalloc(1, sizeof *ui);
	ui->index = utf8_next_index++;
	RB_INSERT(utf8_index_tree, &utf8_index_tree, ui);

	memcpy(ui->data, data, size);
	ui->size = size;
	RB_INSERT(utf8_data_tree, &utf8_data_tree, ui);

	*index = ui->index;
	log_debug("%s: added %.*s = %u", __func__, (int)size, data, *index);
	return (0);
}

/* Get UTF-8 character from data. */
enum utf8_state
utf8_from_data(const struct utf8_data *ud, utf8_char *uc)
{
	u_int	index;

	if (ud->width > 2)
		fatalx("invalid UTF-8 width: %u", ud->width);

	if (ud->size > UTF8_SIZE)
		goto fail;
	if (ud->size <= 3) {
		index = (((utf8_char)ud->data[2] << 16)|
		          ((utf8_char)ud->data[1] << 8)|
		          ((utf8_char)ud->data[0]));
	} else if (utf8_put_item(ud->data, ud->size, &index) != 0)
		goto fail;
	*uc = UTF8_SET_SIZE(ud->size)|UTF8_SET_WIDTH(ud->width)|index;
	log_debug("%s: (%d %d %.*s) -> %08x", __func__, ud->width, ud->size,
	    (int)ud->size, ud->data, *uc);
	return (UTF8_DONE);

fail:
	if (ud->width == 0)
		*uc = UTF8_SET_SIZE(0)|UTF8_SET_WIDTH(0);
	else if (ud->width == 1)
		*uc = UTF8_SET_SIZE(1)|UTF8_SET_WIDTH(1)|0x20;
	else
		*uc = UTF8_SET_SIZE(1)|UTF8_SET_WIDTH(1)|0x2020;
	return (UTF8_ERROR);
}

/* Get UTF-8 data from character. */
void
utf8_to_data(utf8_char uc, struct utf8_data *ud)
{
	struct utf8_item	*ui;
	u_int			 index;

	memset(ud, 0, sizeof *ud);
	ud->size = ud->have = UTF8_GET_SIZE(uc);
	ud->width = UTF8_GET_WIDTH(uc);

	if (ud->size <= 3) {
		ud->data[2] = (uc >> 16);
		ud->data[1] = ((uc >> 8) & 0xff);
		ud->data[0] = (uc & 0xff);
	} else {
		index = (uc & 0xffffff);
		if ((ui = utf8_item_by_index(index)) == NULL)
			memset(ud->data, ' ', ud->size);
		else
			memcpy(ud->data, ui->data, ud->size);
	}

	log_debug("%s: %08x -> (%d %d %.*s)", __func__, uc, ud->width, ud->size,
	    (int)ud->size, ud->data);
}

/* Get UTF-8 character from a single ASCII character. */
u_int
utf8_build_one(u_char ch)
{
	return (UTF8_SET_SIZE(1)|UTF8_SET_WIDTH(1)|ch);
}

/* Set a single character. */
void
utf8_set(struct utf8_data *ud, u_char ch)
{
	static const struct utf8_data empty = { { 0 }, 1, 1, 1 };

	memcpy(ud, &empty, sizeof *ud);
	*ud->data = ch;
}

/* Copy UTF-8 character. */
void
utf8_copy(struct utf8_data *to, const struct utf8_data *from)
{
	u_int	i;

	memcpy(to, from, sizeof *to);

	for (i = to->size; i < sizeof to->data; i++)
		to->data[i] = '\0';
}

/* Get width of Unicode character. */
static enum utf8_state
utf8_width(struct utf8_data *ud, int *width)
{
	wchar_t	wc;

#ifdef HAVE_UTF8PROC
	switch (utf8proc_mbtowc(&wc, ud->data, ud->size)) {
#else
	switch (mbtowc(&wc, ud->data, ud->size)) {
#endif
	case -1:
		log_debug("UTF-8 %.*s, mbtowc() %d", (int)ud->size, ud->data,
		    errno);
		mbtowc(NULL, NULL, MB_CUR_MAX);
		return (UTF8_ERROR);
	case 0:
		return (UTF8_ERROR);
	}
#ifdef HAVE_UTF8PROC
	*width = utf8proc_wcwidth(wc);
#else
	*width = wcwidth(wc);
#endif
	if (options_get_number(global_options, "unicode-ambiguous-width") == 1)
	{
		if (wc == 0x00a1 ||
		    wc == 0x00a4 ||
		    wc == 0x00a7 ||
		    wc == 0x00a8 ||
		    wc == 0x00aa ||
		    wc == 0x00ad ||
		    wc == 0x00ae ||
		    wc == 0x00b0 ||
		    wc == 0x00b1 ||
		    (0x00b2 <= wc && wc <= 0x00b3) ||
		    wc == 0x00b4 ||
		    (0x00b6 <= wc && wc <= 0x00b7) ||
		    wc == 0x00b8 ||
		    wc == 0x00b9 ||
		    wc == 0x00ba ||
		    (0x00bc <= wc && wc <= 0x00be) ||
		    wc == 0x00bf ||
		    wc == 0x00c6 ||
		    wc == 0x00d0 ||
		    wc == 0x00d7 ||
		    wc == 0x00d8 ||
		    (0x00de <= wc && wc <= 0x00e1) ||
		    wc == 0x00e6 ||
		    (0x00e8 <= wc && wc <= 0x00ea) ||
		    (0x00ec <= wc && wc <= 0x00ed) ||
		    wc == 0x00f0 ||
		    (0x00f2 <= wc && wc <= 0x00f3) ||
		    wc == 0x00f7 ||
		    (0x00f8 <= wc && wc <= 0x00fa) ||
		    wc == 0x00fc ||
		    wc == 0x00fe ||
		    wc == 0x0101 ||
		    wc == 0x0111 ||
		    wc == 0x0113 ||
		    wc == 0x011b ||
		    (0x0126 <= wc && wc <= 0x0127) ||
		    wc == 0x012b ||
		    (0x0131 <= wc && wc <= 0x0133) ||
		    wc == 0x0138 ||
		    (0x013f <= wc && wc <= 0x0142) ||
		    wc == 0x0144 ||
		    (0x0148 <= wc && wc <= 0x014b) ||
		    wc == 0x014d ||
		    (0x0152 <= wc && wc <= 0x0153) ||
		    (0x0166 <= wc && wc <= 0x0167) ||
		    wc == 0x016b ||
		    wc == 0x01ce ||
		    wc == 0x01d0 ||
		    wc == 0x01d2 ||
		    wc == 0x01d4 ||
		    wc == 0x01d6 ||
		    wc == 0x01d8 ||
		    wc == 0x01da ||
		    wc == 0x01dc ||
		    wc == 0x0251 ||
		    wc == 0x0261 ||
		    wc == 0x02c4 ||
		    wc == 0x02c7 ||
		    (0x02c9 <= wc && wc <= 0x02cb) ||
		    wc == 0x02cd ||
		    wc == 0x02d0 ||
		    (0x02d8 <= wc && wc <= 0x02db) ||
		    wc == 0x02dd ||
		    wc == 0x02df ||
		    (0x0300 <= wc && wc <= 0x036f) ||
		    (0x0391 <= wc && wc <= 0x03a1) ||
		    (0x03a3 <= wc && wc <= 0x03a9) ||
		    (0x03b1 <= wc && wc <= 0x03c1) ||
		    (0x03c3 <= wc && wc <= 0x03c9) ||
		    wc == 0x0401 ||
		    (0x0410 <= wc && wc <= 0x044f) ||
		    wc == 0x0451 ||
		    wc == 0x2010 ||
		    (0x2013 <= wc && wc <= 0x2015) ||
		    wc == 0x2016 ||
		    wc == 0x2018 ||
		    wc == 0x2019 ||
		    wc == 0x201c ||
		    wc == 0x201d ||
		    (0x2020 <= wc && wc <= 0x2022) ||
		    (0x2024 <= wc && wc <= 0x2027) ||
		    wc == 0x2030 ||
		    (0x2032 <= wc && wc <= 0x2033) ||
		    wc == 0x2035 ||
		    wc == 0x203b ||
		    wc == 0x203e ||
		    wc == 0x2074 ||
		    wc == 0x207f ||
		    (0x2081 <= wc && wc <= 0x2084) ||
		    wc == 0x20ac ||
		    wc == 0x2103 ||
		    wc == 0x2105 ||
		    wc == 0x2109 ||
		    wc == 0x2113 ||
		    wc == 0x2116 ||
		    (0x2121 <= wc && wc <= 0x2122) ||
		    wc == 0x2126 ||
		    wc == 0x212b ||
		    (0x2153 <= wc && wc <= 0x2154) ||
		    (0x215b <= wc && wc <= 0x215e) ||
		    (0x2160 <= wc && wc <= 0x216b) ||
		    (0x2170 <= wc && wc <= 0x2179) ||
		    wc == 0x2189 ||
		    (0x2190 <= wc && wc <= 0x2194) ||
		    (0x2195 <= wc && wc <= 0x2199) ||
		    (0x21b8 <= wc && wc <= 0x21b9) ||
		    wc == 0x21d2 ||
		    wc == 0x21d4 ||
		    wc == 0x21e7 ||
		    wc == 0x2200 ||
		    (0x2202 <= wc && wc <= 0x2203) ||
		    (0x2207 <= wc && wc <= 0x2208) ||
		    wc == 0x220b ||
		    wc == 0x220f ||
		    wc == 0x2211 ||
		    wc == 0x2215 ||
		    wc == 0x221a ||
		    (0x221d <= wc && wc <= 0x2220) ||
		    wc == 0x2223 ||
		    wc == 0x2225 ||
		    (0x2227 <= wc && wc <= 0x222c) ||
		    wc == 0x222e ||
		    (0x2234 <= wc && wc <= 0x2237) ||
		    (0x223c <= wc && wc <= 0x223d) ||
		    wc == 0x2248 ||
		    wc == 0x224c ||
		    wc == 0x2252 ||
		    (0x2260 <= wc && wc <= 0x2261) ||
		    (0x2264 <= wc && wc <= 0x2267) ||
		    (0x226a <= wc && wc <= 0x226b) ||
		    (0x226e <= wc && wc <= 0x226f) ||
		    (0x2282 <= wc && wc <= 0x2283) ||
		    (0x2286 <= wc && wc <= 0x2287) ||
		    wc == 0x2295 ||
		    wc == 0x2299 ||
		    wc == 0x22a5 ||
		    wc == 0x22bf ||
		    wc == 0x2312 ||
		    (0x2460 <= wc && wc <= 0x249b) ||
		    (0x249c <= wc && wc <= 0x24e9) ||
		    (0x24eb <= wc && wc <= 0x24ff) ||
		    (0x2500 <= wc && wc <= 0x254b) ||
		    (0x2550 <= wc && wc <= 0x2573) ||
		    (0x2580 <= wc && wc <= 0x258f) ||
		    (0x2592 <= wc && wc <= 0x2595) ||
		    (0x25a0 <= wc && wc <= 0x25a1) ||
		    (0x25a3 <= wc && wc <= 0x25a9) ||
		    (0x25b2 <= wc && wc <= 0x25b3) ||
		    wc == 0x25b6 ||
		    wc == 0x25b7 ||
		    (0x25bc <= wc && wc <= 0x25bd) ||
		    wc == 0x25c0 ||
		    wc == 0x25c1 ||
		    (0x25c6 <= wc && wc <= 0x25c8) ||
		    wc == 0x25cb ||
		    (0x25ce <= wc && wc <= 0x25d1) ||
		    (0x25e2 <= wc && wc <= 0x25e5) ||
		    wc == 0x25ef ||
		    (0x2605 <= wc && wc <= 0x2606) ||
		    wc == 0x2609 ||
		    (0x260e <= wc && wc <= 0x260f) ||
		    wc == 0x261c ||
		    wc == 0x261e ||
		    wc == 0x2640 ||
		    wc == 0x2642 ||
		    (0x2660 <= wc && wc <= 0x2661) ||
		    (0x2663 <= wc && wc <= 0x2665) ||
		    (0x2667 <= wc && wc <= 0x266a) ||
		    (0x266c <= wc && wc <= 0x266d) ||
		    wc == 0x266f ||
		    (0x269e <= wc && wc <= 0x269f) ||
		    wc == 0x26bf ||
		    (0x26c6 <= wc && wc <= 0x26cd) ||
		    (0x26cf <= wc && wc <= 0x26d3) ||
		    (0x26d5 <= wc && wc <= 0x26e1) ||
		    wc == 0x26e3 ||
		    (0x26e8 <= wc && wc <= 0x26e9) ||
		    (0x26eb <= wc && wc <= 0x26f1) ||
		    wc == 0x26f4 ||
		    (0x26f6 <= wc && wc <= 0x26f9) ||
		    (0x26fb <= wc && wc <= 0x26fc) ||
		    (0x26fe <= wc && wc <= 0x26ff) ||
		    wc == 0x273d ||
		    (0x2776 <= wc && wc <= 0x277f) ||
		    (0x2b56 <= wc && wc <= 0x2b59) ||
		    (0x3248 <= wc && wc <= 0x324f) ||
		    (0xe000 <= wc && wc <= 0xf8ff) ||
		    (0xfe00 <= wc && wc <= 0xfe0f) ||
		    wc == 0xfffd)
			if (*width == 1)
				*width = 2;
	}
	if (*width >= 0 && *width <= 0xff)
		return (UTF8_DONE);
	log_debug("UTF-8 %.*s, wcwidth() %d", (int)ud->size, ud->data, *width);

#ifndef __OpenBSD__
	/*
	 * Many platforms (particularly and inevitably OS X) have no width for
	 * relatively common characters (wcwidth() returns -1); assume width 1
	 * in this case. This will be wrong for genuinely nonprintable
	 * characters, but they should be rare. We may pass through stuff that
	 * ideally we would block, but this is no worse than sending the same
	 * to the terminal without tmux.
	 */
	if (*width < 0) {
		*width = 1;
		return (UTF8_DONE);
	}
#endif
	return (UTF8_ERROR);
}

/*
 * Open UTF-8 sequence.
 *
 * 11000010-11011111 C2-DF start of 2-byte sequence
 * 11100000-11101111 E0-EF start of 3-byte sequence
 * 11110000-11110100 F0-F4 start of 4-byte sequence
 */
enum utf8_state
utf8_open(struct utf8_data *ud, u_char ch)
{
	memset(ud, 0, sizeof *ud);
	if (ch >= 0xc2 && ch <= 0xdf)
		ud->size = 2;
	else if (ch >= 0xe0 && ch <= 0xef)
		ud->size = 3;
	else if (ch >= 0xf0 && ch <= 0xf4)
		ud->size = 4;
	else
		return (UTF8_ERROR);
	utf8_append(ud, ch);
	return (UTF8_MORE);
}

/* Append character to UTF-8, closing if finished. */
enum utf8_state
utf8_append(struct utf8_data *ud, u_char ch)
{
	int	width;

	if (ud->have >= ud->size)
		fatalx("UTF-8 character overflow");
	if (ud->size > sizeof ud->data)
		fatalx("UTF-8 character size too large");

	if (ud->have != 0 && (ch & 0xc0) != 0x80)
		ud->width = 0xff;

	ud->data[ud->have++] = ch;
	if (ud->have != ud->size)
		return (UTF8_MORE);

	if (ud->width == 0xff)
		return (UTF8_ERROR);
	if (utf8_width(ud, &width) != UTF8_DONE)
		return (UTF8_ERROR);
	ud->width = width;

	return (UTF8_DONE);
}

/*
 * Encode len characters from src into dst, which is guaranteed to have four
 * bytes available for each character from src (for \abc or UTF-8) plus space
 * for \0.
 */
int
utf8_strvis(char *dst, const char *src, size_t len, int flag)
{
	struct utf8_data	 ud;
	const char		*start = dst, *end = src + len;
	enum utf8_state		 more;
	size_t			 i;

	while (src < end) {
		if ((more = utf8_open(&ud, *src)) == UTF8_MORE) {
			while (++src < end && more == UTF8_MORE)
				more = utf8_append(&ud, *src);
			if (more == UTF8_DONE) {
				/* UTF-8 character finished. */
				for (i = 0; i < ud.size; i++)
					*dst++ = ud.data[i];
				continue;
			}
			/* Not a complete, valid UTF-8 character. */
			src -= ud.have;
		}
		if (src[0] == '$' && src < end - 1) {
			if (isalpha((u_char)src[1]) ||
			    src[1] == '_' ||
			    src[1] == '{')
				*dst++ = '\\';
			*dst++ = '$';
		} else if (src < end - 1)
			dst = vis(dst, src[0], flag, src[1]);
		else if (src < end)
			dst = vis(dst, src[0], flag, '\0');
		src++;
	}
	*dst = '\0';
	return (dst - start);
}

/* Same as utf8_strvis but allocate the buffer. */
int
utf8_stravis(char **dst, const char *src, int flag)
{
	char	*buf;
	int	 len;

	buf = xreallocarray(NULL, 4, strlen(src) + 1);
	len = utf8_strvis(buf, src, strlen(src), flag);

	*dst = xrealloc(buf, len + 1);
	return (len);
}

/* Same as utf8_strvis but allocate the buffer. */
int
utf8_stravisx(char **dst, const char *src, size_t srclen, int flag)
{
	char	*buf;
	int	 len;

	buf = xreallocarray(NULL, 4, srclen + 1);
	len = utf8_strvis(buf, src, srclen, flag);

	*dst = xrealloc(buf, len + 1);
	return (len);
}

/* Does this string contain anything that isn't valid UTF-8? */
int
utf8_isvalid(const char *s)
{
	struct utf8_data ud;
	const char	*end;
	enum utf8_state	 more;

	end = s + strlen(s);
	while (s < end) {
		if ((more = utf8_open(&ud, *s)) == UTF8_MORE) {
			while (++s < end && more == UTF8_MORE)
				more = utf8_append(&ud, *s);
			if (more == UTF8_DONE)
				continue;
			return (0);
		}
		if (*s < 0x20 || *s > 0x7e)
			return (0);
		s++;
	}
	return (1);
}

/*
 * Sanitize a string, changing any UTF-8 characters to '_'. Caller should free
 * the returned string. Anything not valid printable ASCII or UTF-8 is
 * stripped.
 */
char *
utf8_sanitize(const char *src)
{
	char		*dst = NULL;
	size_t		 n = 0;
	enum utf8_state	 more;
	struct utf8_data ud;
	u_int		 i;

	while (*src != '\0') {
		dst = xreallocarray(dst, n + 1, sizeof *dst);
		if ((more = utf8_open(&ud, *src)) == UTF8_MORE) {
			while (*++src != '\0' && more == UTF8_MORE)
				more = utf8_append(&ud, *src);
			if (more == UTF8_DONE) {
				dst = xreallocarray(dst, n + ud.width,
				    sizeof *dst);
				for (i = 0; i < ud.width; i++)
					dst[n++] = '_';
				continue;
			}
			src -= ud.have;
		}
		if (*src > 0x1f && *src < 0x7f)
			dst[n++] = *src;
		else
			dst[n++] = '_';
		src++;
	}
	dst = xreallocarray(dst, n + 1, sizeof *dst);
	dst[n] = '\0';
	return (dst);
}

/* Get UTF-8 buffer length. */
size_t
utf8_strlen(const struct utf8_data *s)
{
	size_t	i;

	for (i = 0; s[i].size != 0; i++)
		/* nothing */;
	return (i);
}

/* Get UTF-8 string width. */
u_int
utf8_strwidth(const struct utf8_data *s, ssize_t n)
{
	ssize_t	i;
	u_int	width = 0;

	for (i = 0; s[i].size != 0; i++) {
		if (n != -1 && n == i)
			break;
		width += s[i].width;
	}
	return (width);
}

/*
 * Convert a string into a buffer of UTF-8 characters. Terminated by size == 0.
 * Caller frees.
 */
struct utf8_data *
utf8_fromcstr(const char *src)
{
	struct utf8_data	*dst = NULL;
	size_t			 n = 0;
	enum utf8_state		 more;

	while (*src != '\0') {
		dst = xreallocarray(dst, n + 1, sizeof *dst);
		if ((more = utf8_open(&dst[n], *src)) == UTF8_MORE) {
			while (*++src != '\0' && more == UTF8_MORE)
				more = utf8_append(&dst[n], *src);
			if (more == UTF8_DONE) {
				n++;
				continue;
			}
			src -= dst[n].have;
		}
		utf8_set(&dst[n], *src);
		n++;
		src++;
	}
	dst = xreallocarray(dst, n + 1, sizeof *dst);
	dst[n].size = 0;
	return (dst);
}

/* Convert from a buffer of UTF-8 characters into a string. Caller frees. */
char *
utf8_tocstr(struct utf8_data *src)
{
	char	*dst = NULL;
	size_t	 n = 0;

	for(; src->size != 0; src++) {
		dst = xreallocarray(dst, n + src->size, 1);
		memcpy(dst + n, src->data, src->size);
		n += src->size;
	}
	dst = xreallocarray(dst, n + 1, 1);
	dst[n] = '\0';
	return (dst);
}

/* Get width of UTF-8 string. */
u_int
utf8_cstrwidth(const char *s)
{
	struct utf8_data	tmp;
	u_int			width;
	enum utf8_state		more;

	width = 0;
	while (*s != '\0') {
		if ((more = utf8_open(&tmp, *s)) == UTF8_MORE) {
			while (*++s != '\0' && more == UTF8_MORE)
				more = utf8_append(&tmp, *s);
			if (more == UTF8_DONE) {
				width += tmp.width;
				continue;
			}
			s -= tmp.have;
		}
		if (*s > 0x1f && *s != 0x7f)
			width++;
		s++;
	}
	return (width);
}

/* Pad UTF-8 string to width on the left. Caller frees. */
char *
utf8_padcstr(const char *s, u_int width)
{
	size_t	 slen;
	char	*out;
	u_int	 n, i;

	n = utf8_cstrwidth(s);
	if (n >= width)
		return (xstrdup(s));

	slen = strlen(s);
	out = xmalloc(slen + 1 + (width - n));
	memcpy(out, s, slen);
	for (i = n; i < width; i++)
		out[slen++] = ' ';
	out[slen] = '\0';
	return (out);
}

/* Pad UTF-8 string to width on the right. Caller frees. */
char *
utf8_rpadcstr(const char *s, u_int width)
{
	size_t	 slen;
	char	*out;
	u_int	 n, i;

	n = utf8_cstrwidth(s);
	if (n >= width)
		return (xstrdup(s));

	slen = strlen(s);
	out = xmalloc(slen + 1 + (width - n));
	for (i = 0; i < width - n; i++)
		out[i] = ' ';
	memcpy(out + i, s, slen);
	out[i + slen] = '\0';
	return (out);
}

int
utf8_cstrhas(const char *s, const struct utf8_data *ud)
{
	struct utf8_data	*copy, *loop;
	int			 found = 0;

	copy = utf8_fromcstr(s);
	for (loop = copy; loop->size != 0; loop++) {
		if (loop->size != ud->size)
			continue;
		if (memcmp(loop->data, ud->data, loop->size) == 0) {
			found = 1;
			break;
		}
	}
	free(copy);

	return (found);
}
