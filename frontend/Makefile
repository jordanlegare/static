CXX ?= c++
PKG_CONFIG ?= pkg-config
GTK4_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk4)
GTK4_LIBS := $(shell $(PKG_CONFIG) --libs gtk4)
CPPFLAGS += -I../include $(GTK4_CFLAGS)
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic

.PHONY: all clean
all: staticbus-gtk staticbus-telemetry-gtk

staticbus-gtk: staticbus-gtk.cpp ../include/staticbus_ioctl.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(GTK4_LIBS)

staticbus-telemetry-gtk: staticbus-telemetry-gtk.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(GTK4_LIBS)

clean:
	rm -f staticbus-gtk staticbus-telemetry-gtk
