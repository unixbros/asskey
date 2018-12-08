#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <err.h>

typedef struct font_s {
	xcb_font_t ptr;
	int descent, height, width;
	uint16_t char_max;
	uint16_t char_min;
	xcb_charinfo_t *width_lut;
} font_t;

typedef struct term_s {
	int width, height;
} term_t;

static xcb_gcontext_t gc;
static xcb_connection_t *conn;
static xcb_screen_t *scr;
static xcb_window_t termwin;
static xcb_window_t palettewin;
static font_t *font;
static term_t term;

/*
 * thanks for wmdia for doing what xcb devs can't
 */
xcb_void_cookie_t
xcb_poly_text_16_simple(xcb_connection_t *c, xcb_drawable_t drawable,
		xcb_gcontext_t gc, int16_t x, int16_t y, uint32_t len,
		const uint16_t *str)
{
	struct iovec xcb_parts[7];
	static const xcb_protocol_request_t xcb_req = {
		5,                /* count  */
		0,                /* ext    */
		XCB_POLY_TEXT_16, /* opcode */
		1                 /* isvoid */
	};
	uint8_t xcb_lendelta[2];
	xcb_void_cookie_t xcb_ret;
	xcb_poly_text_8_request_t xcb_out;

	xcb_out.pad0 = 0;
	xcb_out.drawable = drawable;
	xcb_out.gc = gc;
	xcb_out.x = x;
	xcb_out.y = y;

	xcb_lendelta[0] = len;
	xcb_lendelta[1] = 0;

	xcb_parts[2].iov_base = (char *)&xcb_out;
	xcb_parts[2].iov_len = sizeof(xcb_out);
	xcb_parts[3].iov_base = 0;
	xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

	xcb_parts[4].iov_base = xcb_lendelta;
	xcb_parts[4].iov_len = sizeof(xcb_lendelta);
	xcb_parts[5].iov_base = (char *)str;
	xcb_parts[5].iov_len = len * sizeof(int16_t);

	xcb_parts[6].iov_base = 0;
	xcb_parts[6].iov_len = -(xcb_parts[4].iov_len + xcb_parts[5].iov_len) & 3;

	xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);

	return xcb_ret;
}

void
fill_rect(int x, int y, int color) {
	xcb_rectangle_t rect;
	xcb_gcontext_t gc;
	uint32_t mask, values[1];
	int rx, ry;

	gc = xcb_generate_id(conn);
	mask = XCB_GC_FOREGROUND;
	values[0] = color;

	xcb_create_gc(conn, gc, termwin, mask, values);

	rx = x * font->width;
	ry = y * font->height;

	rect.x = rx;
	rect.y = ry;
	rect.width = font->width;
	rect.height = font->height;

	xcb_poly_fill_rectangle(conn, termwin, gc, 1, &rect);
}

font_t *
load_font(const char *name) {
	xcb_query_font_cookie_t queryreq;
	xcb_query_font_reply_t *info;
	xcb_void_cookie_t cookie;
	xcb_font_t font;
	font_t *ret;

	font = xcb_generate_id(conn);

	cookie = xcb_open_font_checked(conn, font, strlen(name), name);
	if (xcb_request_check(conn, cookie)) {
		warnx("could not load font '%s'", name);
		return NULL;
	}

	ret = malloc(sizeof(font_t));
	if (ret == NULL)
		return NULL;

	queryreq = xcb_query_font(conn, font);
	info = xcb_query_font_reply(conn, queryreq, NULL);

	ret->ptr = font;
	ret->descent = info->font_descent;
	ret->height = info->font_ascent + info->font_descent;
	ret->width = info->max_bounds.character_width;
	ret->char_max = info->max_byte1 << 8 | info->max_char_or_byte2;
	ret->char_min = info->min_byte1 << 8 | info->min_char_or_byte2;

	xcb_change_gc(conn, gc, XCB_GC_FONT, &font);

	return ret;
}

int
utf_len(char *str) {
	uint8_t *utf = (uint8_t *)str;

	if (utf[0] < 0x80)
		return 1;
	else if ((utf[0] & 0xe0) == 0xc0)
		return 2;
	else if ((utf[0] & 0xf0) == 0xe0)
		return 3;
	else if ((utf[0] & 0xf8) == 0xf0)
		return 4;
	else if ((utf[0] & 0xfc) == 0xf8)
		return 5;
	else if ((utf[0] & 0xfe) == 0xfc)
		return 6;
	else
		return 1;
}

void
set_cell(int x, int y, xcb_window_t win, char *str) {
	uint16_t chr;
	uint8_t *utf = (uint8_t *)str;
	int rx, ry;


	switch (utf_len(str)) {
	case 1:
		chr = utf[0];
		break;
	case 2:
		chr = (utf[0] & 0x1f) << 6 | (utf[1] & 0x3f);
		break;
	case 3:
		chr = (utf[0] & 0xf) << 12 | (utf[1] & 0x3f) << 6 | (utf[2] & 0x3f);
		break;
	case 4:
	case 5:
	case 6:
		chr = 0xfffd;
		break;
	}

	chr = chr >> 8 | chr << 8;

	rx = (x + 1) * font->width;
	ry = (y + 1) * (font->height - font->descent);

	xcb_poly_text_16_simple(conn, win, gc, rx, ry, 1, &chr);
}

void
resize_win(xcb_window_t win, int x, int y) {
	uint32_t values[3];
	uint32_t mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

	values[0] = font->width * x;
	values[1] = font->height * y;

	term.width = x;
	term.height = y;

	xcb_configure_window(conn, win, mask, values);
}

int
xcb_puts(int x, int y, xcb_window_t win, char *str, int color) {
	char *utf = str;
	int rx, ry;
	uint32_t mask, values[2];

	rx = x;
	ry = y;

	mask = XCB_GC_FOREGROUND;
	values[0] = color;
	xcb_change_gc(conn, gc, mask, values);

	while (*utf) {
		set_cell(rx, ry, win, utf);
		utf += utf_len(utf);
		rx++;
		if (rx >= term.width - 1) {
			rx = 0;
			ry++;
		}

		if (ry >= term.height - 1)
			return 1;
	}

	return 0;
}

void
xinit(void) {
	uint32_t mask;
	uint32_t values[3];

	conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(conn))
		err(1, "xcb connection has error");

	scr = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;

        mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        values[0] = scr->black_pixel;
        values[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_MOTION;

	termwin = xcb_generate_id(conn);
        xcb_create_window (conn,
                           XCB_COPY_FROM_PARENT,
                           termwin,
                           scr->root,
                           0, 0,
                           150, 150,
                           10,
                           XCB_WINDOW_CLASS_INPUT_OUTPUT,
                           scr->root_visual,
                           mask, values );

	xcb_map_window(conn, termwin);
	xcb_flush(conn);

	palettewin = xcb_generate_id(conn);
        xcb_create_window (conn,
                           XCB_COPY_FROM_PARENT,
                           palettewin,
                           scr->root,
                           0, 0,
                           100, 500,
                           10,
                           XCB_WINDOW_CLASS_INPUT_OUTPUT,
                           scr->root_visual,
                           mask, values );

	xcb_map_window(conn, palettewin);

	mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
        values[0] = 0xff00ff;
	values[1] = 0xff00ff;
	values[2] = 0;

	gc = xcb_generate_id(conn);
        xcb_create_gc(conn, gc, termwin, mask, values);

	font = load_font("-*-gohufont-medium-*-*-*-11-*-*-*-*-*-*-1");

	mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	values[0] = font->width * term.width;
	values[1] = font->height * term.height;

	xcb_configure_window(conn, termwin, mask, values);

	xcb_flush(conn);
}

int
main(int argc, char **argv) {
	term.width = 80;
	term.height = 24;

	xinit();
	resize_win(termwin, 80, 24);
	resize_win(palettewin, 20, 40);

	xcb_generic_event_t *ev;
	while ((ev = xcb_wait_for_event (conn))) {
		switch (ev->response_type & ~0x80) {
		case XCB_EXPOSE:
			xcb_puts(0, 0, termwin, "test", 0xffff00);
			xcb_puts(0, 0, palettewin, "palette", 0xffffff);
			xcb_flush(conn);
			/* setup */
			break;
		case XCB_BUTTON_PRESS: {
			xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;

			if (e->event == palettewin)
				puts("set color or char here");

			if (e->event == termwin)
				puts("draw to canvas here");
		} break;
		case XCB_BUTTON_RELEASE:
			break;
		case XCB_MOTION_NOTIFY:
			break;
		}
	}

	xcb_disconnect(conn);
	return 0;
}
