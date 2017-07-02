#include "wayland-desktop-shell-client-protocol.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <wayland-client.h>
#include <time.h>
#include <string.h>
#include "client/window.h"
#include "client/registry.h"
#include "client/cairo.h"
#include "log.h"
#include "list.h"
#include "util.h"

list_t *surfaces;
struct registry *registry;

enum scaling_mode {
	SCALING_MODE_STRETCH,
	SCALING_MODE_FILL,
	SCALING_MODE_FIT,
	SCALING_MODE_CENTER,
	SCALING_MODE_TILE,
};

void sway_terminate(int exit_code) {
	int i;
	for (i = 0; i < surfaces->length; ++i) {
		struct window *window = surfaces->items[i];
		window_teardown(window);
	}
	list_free(surfaces);
	registry_teardown(registry);
	exit(exit_code);
}

bool is_valid_color(const char *color) {
	int len = strlen(color);
	if (len != 7 || color[0] != '#') {
		sway_log(L_ERROR, "%s is not a valid color for swaybg. Color should be specified as #rrggbb (no alpha).", color);
		return false;
	}

	int i;
	for (i = 1; i < len; ++i) {
		if (!isxdigit(color[i])) {
			return false;
		}
	}

	return true;
}

int main(int argc, const char **argv) {
	init_log(L_INFO);
	surfaces = create_list();
	registry = registry_poll();

	if (argc != 4) {
		sway_abort("Do not run this program manually. See man 5 sway and look for output options.");
	}

	if (!registry->desktop_shell) {
		sway_abort("swaybg requires the compositor to support the desktop-shell extension.");
	}

	int desired_output = atoi(argv[1]);
	sway_log(L_INFO, "Using output %d of %d", desired_output, registry->outputs->length);
	int i;
	struct output_state *output = registry->outputs->items[desired_output];
	struct window *window = window_setup(registry,
			output->width, output->height, output->scale, false);
	if (!window) {
		sway_abort("Failed to create surfaces.");
	}
	desktop_shell_set_background(registry->desktop_shell, output->output, window->surface);
	window_make_shell(window);
	list_add(surfaces, window);

	if (strcmp(argv[3], "solid_color") == 0 && is_valid_color(argv[2])) {
		cairo_set_source_u32(window->cairo, parse_color(argv[2]));
		cairo_paint(window->cairo);
		window_render(window);
	} else {
#ifdef WITH_GDK_PIXBUF
		GError *err = NULL;
		GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(argv[2], &err);
		if (!pixbuf) {
			sway_abort("Failed to load background image.");
		}
		cairo_surface_t *image = gdk_cairo_image_surface_create_from_pixbuf(pixbuf);
		g_object_unref(pixbuf);
#else
		cairo_surface_t *image = cairo_image_surface_create_from_png(argv[2]);
#endif //WITH_GDK_PIXBUF
		if (!image) {
			sway_abort("Failed to read background image.");
		}
		if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
			sway_abort("Failed to read background image: %s", cairo_status_to_string(cairo_surface_status(image)));
		}
		double width = cairo_image_surface_get_width(image);
		double height = cairo_image_surface_get_height(image);

		const char *scaling_mode_str = argv[3];
		enum scaling_mode scaling_mode = SCALING_MODE_STRETCH;
		if (strcmp(scaling_mode_str, "stretch") == 0) {
			scaling_mode = SCALING_MODE_STRETCH;
		} else if (strcmp(scaling_mode_str, "fill") == 0) {
			scaling_mode = SCALING_MODE_FILL;
		} else if (strcmp(scaling_mode_str, "fit") == 0) {
			scaling_mode = SCALING_MODE_FIT;
		} else if (strcmp(scaling_mode_str, "center") == 0) {
			scaling_mode = SCALING_MODE_CENTER;
		} else if (strcmp(scaling_mode_str, "tile") == 0) {
			scaling_mode = SCALING_MODE_TILE;
		} else {
			sway_abort("Unsupported scaling mode: %s", scaling_mode_str);
		}

		int wwidth = window->width * window->scale;
		int wheight = window->height * window->scale;

		for (i = 0; i < surfaces->length; ++i) {
			struct window *window = surfaces->items[i];
			if (window_prerender(window) && window->cairo) {
				switch (scaling_mode) {
				case SCALING_MODE_STRETCH:
					cairo_scale(window->cairo,
							(double) wwidth / width,
							(double) wheight / height);
					cairo_set_source_surface(window->cairo, image, 0, 0);
					break;
				case SCALING_MODE_FILL:
				{
					double window_ratio = (double) wwidth / wheight;
					double bg_ratio = width / height;

					if (window_ratio > bg_ratio) {
						double scale = (double) wwidth / width;
						cairo_scale(window->cairo, scale, scale);
						cairo_set_source_surface(window->cairo, image,
								0,
								(double) wheight/2 / scale - height/2);
					} else {
						double scale = (double) wheight / height;
						cairo_scale(window->cairo, scale, scale);
						cairo_set_source_surface(window->cairo, image,
								(double) wwidth/2 / scale - width/2,
								0);
					}
					break;
				}
				case SCALING_MODE_FIT:
				{
					double window_ratio = (double) wwidth / wheight;
					double bg_ratio = width / height;

					if (window_ratio > bg_ratio) {
						double scale = (double) wheight / height;
						cairo_scale(window->cairo, scale, scale);
						cairo_set_source_surface(window->cairo, image,
								(double) wwidth/2 / scale - width/2,
								0);
					} else {
						double scale = (double) wwidth / width;
						cairo_scale(window->cairo, scale, scale);
						cairo_set_source_surface(window->cairo, image,
								0,
								(double) wheight/2 / scale - height/2);
					}
					break;
				}
				case SCALING_MODE_CENTER:
					cairo_set_source_surface(window->cairo, image,
							(double) wwidth/2 - width/2,
							(double) wheight/2 - height/2);
					break;
				case SCALING_MODE_TILE:
				{
					cairo_pattern_t *pattern = cairo_pattern_create_for_surface(image);
					cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
					cairo_set_source(window->cairo, pattern);
					break;
				}
				default:
					sway_abort("Scaling mode '%s' not implemented yet!", scaling_mode_str);
				}

				cairo_paint(window->cairo);

				window_render(window);
			}
		}

		cairo_surface_destroy(image);
	}

	while (wl_display_dispatch(registry->display) != -1);

	for (i = 0; i < surfaces->length; ++i) {
		struct window *window = surfaces->items[i];
		window_teardown(window);
	}
	list_free(surfaces);
	registry_teardown(registry);
	return 0;
}
