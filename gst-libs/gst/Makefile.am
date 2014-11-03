
if USE_OPENGL
GL_DIR = gl
endif
if USE_GLES2
GL_DIR = gl
endif

if USE_WAYLAND
WAYLAND_DIR=wayland
endif

if HAVE_EGL
EGL_DIR = egl
endif

SUBDIRS = interfaces basecamerabinsrc codecparsers \
	 insertbin uridownloader mpegts base video $(GL_DIR) $(WAYLAND_DIR) $(EGL_DIR)

noinst_HEADERS = gst-i18n-plugin.h gettext.h glib-compat-private.h
DIST_SUBDIRS = interfaces egl gl  signalprocessorbasecamerabinsrc codecparsers \
	insertbin uridownloader mpegts wayland base video

