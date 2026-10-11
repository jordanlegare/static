CXX ?= c++
PKG_CONFIG ?= pkg-config
TARGET := staticbus-gtk
SOURCES := staticbus-gtk.cpp
CPPFLAGS += -I../include $(shell $(PKG_CONFIG) --cflags gtk4)
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs gtk4)

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SOURCES) ../include/staticbus_ioctl.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f $(TARGET)
