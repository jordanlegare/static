// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtk/gtk.h>
#include <cairo.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <array>
#include <algorithm>
#include <sstream>
#include <iomanip>

#include "staticbus_ioctl.h"

namespace {
constexpr std::size_t kHistoryLimit = 2048;
constexpr std::size_t kTextLimitChars = 90000;

struct AppState {
    std::string device{ "/dev/staticbus0" };
    int fd{-1};
    GtkWidget *window{};
    GtkWidget *status_label{};
    GtkWidget *stats_label{};
    GtkWidget *chart{};
    GtkWidget *hex_view{};
    GtkWidget *pause_button{};
    std::deque<std::uint8_t> history;
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t observed_bytes{0};
    std::uint64_t bit_changes{0};
    std::uint64_t comparisons{0};
    int previous{-1};
    bool paused{false};
};

void set_status(AppState *state, const std::string &message, bool is_error = false)
{
    gtk_label_set_text(GTK_LABEL(state->status_label), message.c_str());
    gtk_widget_add_css_class(state->status_label, is_error ? "error" : "dim-label");
    if (is_error)
        gtk_widget_remove_css_class(state->status_label, "dim-label");
    else
        gtk_widget_remove_css_class(state->status_label, "error");
}

void close_device(AppState *state)
{
    if (state->fd >= 0) {
        close(state->fd);
        state->fd = -1;
    }
}

bool open_device(AppState *state)
{
    if (state->fd >= 0)
        return true;
    state->fd = open(state->device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (state->fd < 0) {
        set_status(state, "Disconnected: " + state->device + " — " +
                          std::strerror(errno), true);
        return false;
    }
    set_status(state, "Connected to " + state->device);
    return true;
}

void clear_history(AppState *state)
{
    state->history.clear();
    state->counts.fill(0);
    state->observed_bytes = 0;
    state->bit_changes = 0;
    state->comparisons = 0;
    state->previous = -1;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->hex_view)),
                             "Waiting for byte stream…\n", -1);
    gtk_widget_queue_draw(state->chart);
}

void add_hex_dump(AppState *state, const std::uint8_t *data, std::size_t size)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->hex_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);

    std::ostringstream out;
    for (std::size_t i = 0; i < size; i += 16) {
        out << std::hex << std::setw(8) << std::setfill('0')
            << (state->observed_bytes - size + i) << "  ";
        const std::size_t line_end = std::min(size, i + 16);
        for (std::size_t j = i; j < line_end; ++j)
            out << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>(data[j]) << ' ';
        out << '\n';
    }
    const std::string text = out.str();
    gtk_text_buffer_insert(buffer, &end, text.c_str(), static_cast<gint>(text.size()));

    gint chars = gtk_text_buffer_get_char_count(buffer);
    if (chars > static_cast<gint>(kTextLimitChars)) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(buffer, &start);
        gtk_text_buffer_get_iter_at_offset(buffer, &cut,
            chars - static_cast<gint>(kTextLimitChars * 3 / 4));
        gtk_text_buffer_delete(buffer, &start, &cut);
    }
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(state->hex_view), &end, 0.0, FALSE, 0.0, 0.0);
}

void process_bytes(AppState *state, const std::uint8_t *data, std::size_t size)
{
    if (!state->paused) {
        for (std::size_t i = 0; i < size; ++i) {
            const std::uint8_t value = data[i];
            if (state->history.size() == kHistoryLimit) {
                --state->counts[state->history.front()];
                state->history.pop_front();
            }
            state->history.push_back(value);
            ++state->counts[value];
            if (state->previous >= 0) {
#if defined(__GNUC__) || defined(__clang__)
                state->bit_changes += static_cast<std::uint64_t>(
                    __builtin_popcount(static_cast<unsigned>(state->previous ^ value)));
#else
                unsigned diff = static_cast<unsigned>(state->previous ^ value);
                while (diff) { state->bit_changes += diff & 1U; diff >>= 1U; }
#endif
                ++state->comparisons;
            }
            state->previous = value;
            ++state->observed_bytes;
        }
        add_hex_dump(state, data, size);
        gtk_widget_queue_draw(state->chart);
    }
}

double entropy_bits(const AppState *state)
{
    if (state->history.empty())
        return 0.0;
    const double total = static_cast<double>(state->history.size());
    double entropy = 0.0;
    for (const auto count : state->counts) {
        if (!count) continue;
        const double p = static_cast<double>(count) / total;
        entropy -= p * std::log2(p);
    }
    return entropy;
}

void chart_draw(GtkDrawingArea *, cairo_t *cr, int width, int height, gpointer data)
{
    auto *state = static_cast<AppState *>(data);
    cairo_set_source_rgb(cr, 0.055, 0.075, 0.10);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 0.7, 0.78, 0.86, 0.16);
    cairo_set_line_width(cr, 1.0);
    for (int row = 1; row < 4; ++row) {
        const double y = static_cast<double>(height) * row / 4.0;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, width, y);
    }
    cairo_stroke(cr);

    const auto max_it = std::max_element(state->counts.begin(), state->counts.end());
    const double max_count = max_it == state->counts.end() || *max_it == 0
                           ? 1.0 : static_cast<double>(*max_it);
    const double bar_width = static_cast<double>(width) / 256.0;
    cairo_set_source_rgb(cr, 0.25, 0.78, 0.67);
    for (std::size_t i = 0; i < state->counts.size(); ++i) {
        const double bar_height = (static_cast<double>(state->counts[i]) / max_count)
                                * (height - 12.0);
        cairo_rectangle(cr, i * bar_width, height - bar_height,
                        std::max(1.0, bar_width - 0.45), bar_height);
    }
    cairo_fill(cr);
}

void update_stats_label(AppState *state)
{
    staticbus_stats stats{};
    if (state->fd >= 0 && ioctl(state->fd, STATICBUS_IOC_GET_STATS, &stats) == 0) {
        const double avg_changes = state->comparisons
            ? static_cast<double>(state->bit_changes) / state->comparisons : 0.0;
        char line[512];
        std::snprintf(line, sizeof(line),
            "Kernel API v%u   FIFO: %u / %u bytes   Received: %llu   Read: %llu\n"
            "Window: %zu / %zu bytes   Shannon entropy: %.3f bits/byte   "
            "Mean changed bits/byte: %.2f",
            stats.abi_version, stats.buffer_used, stats.buffer_capacity,
            static_cast<unsigned long long>(stats.bytes_written),
            static_cast<unsigned long long>(stats.bytes_read),
            state->history.size(), kHistoryLimit, entropy_bits(state), avg_changes);
        gtk_label_set_text(GTK_LABEL(state->stats_label), line);
    } else {
        gtk_label_set_text(GTK_LABEL(state->stats_label),
            "Kernel counters unavailable. Load the module and check device permissions.");
    }
}

gboolean poll_stream(gpointer data)
{
    auto *state = static_cast<AppState *>(data);
    if (!open_device(state)) {
        update_stats_label(state);
        return G_SOURCE_CONTINUE;
    }

    std::uint8_t buffer[4096];
    bool saw_bytes = false;
    for (int turns = 0; turns < 8; ++turns) {
        const ssize_t n = read(state->fd, buffer, sizeof(buffer));
        if (n > 0) {
            saw_bytes = true;
            process_bytes(state, buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            set_status(state, "Device returned end-of-stream", true);
            close_device(state);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            set_status(state, "Read error: " + std::string(std::strerror(errno)), true);
            close_device(state);
        }
        break;
    }
    if (saw_bytes && state->fd >= 0)
        set_status(state, "Receiving byte stream from " + state->device);
    update_stats_label(state);
    return G_SOURCE_CONTINUE;
}

void reconnect_clicked(GtkButton *, gpointer data)
{
    auto *state = static_cast<AppState *>(data);
    close_device(state);
    open_device(state);
}

void pause_clicked(GtkButton *, gpointer data)
{
    auto *state = static_cast<AppState *>(data);
    state->paused = !state->paused;
    gtk_button_set_label(GTK_BUTTON(state->pause_button), state->paused ? "Resume view" : "Pause view");
    set_status(state, state->paused ? "View paused (kernel stream is still drained)"
                                    : "View resumed");
}

void clear_clicked(GtkButton *, gpointer data)
{
    clear_history(static_cast<AppState *>(data));
}

GtkWidget *make_heading(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "title-3");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

void activate(GtkApplication *app, gpointer user_data)
{
    auto *state = static_cast<AppState *>(user_data);
    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "staticbus — Byte Stream Monitor");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 1050, 760);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_window_set_child(GTK_WINDOW(state->window), root);

    GtkWidget *title = gtk_label_new("staticbus");
    gtk_widget_add_css_class(title, "title-1");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), title);

    GtkWidget *subtitle = gtk_label_new("Kernel character device • byte histogram • rolling stream diagnostics");
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), subtitle);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    state->status_label = gtk_label_new("Connecting…");
    gtk_widget_set_halign(state->status_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(state->status_label, TRUE);
    GtkWidget *reconnect = gtk_button_new_with_label("Reconnect");
    state->pause_button = gtk_button_new_with_label("Pause view");
    GtkWidget *clear = gtk_button_new_with_label("Clear view");
    g_signal_connect(reconnect, "clicked", G_CALLBACK(reconnect_clicked), state);
    g_signal_connect(state->pause_button, "clicked", G_CALLBACK(pause_clicked), state);
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_clicked), state);
    gtk_box_append(GTK_BOX(toolbar), state->status_label);
    gtk_box_append(GTK_BOX(toolbar), reconnect);
    gtk_box_append(GTK_BOX(toolbar), state->pause_button);
    gtk_box_append(GTK_BOX(toolbar), clear);
    gtk_box_append(GTK_BOX(root), toolbar);

    gtk_box_append(GTK_BOX(root), make_heading("Byte frequency (rolling 2 KiB window)"));
    state->chart = gtk_drawing_area_new();
    gtk_widget_set_size_request(state->chart, -1, 220);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(state->chart), chart_draw, state, nullptr);
    gtk_box_append(GTK_BOX(root), state->chart);

    gtk_box_append(GTK_BOX(root), make_heading("Stream statistics"));
    state->stats_label = gtk_label_new("Waiting for kernel statistics…");
    gtk_label_set_xalign(GTK_LABEL(state->stats_label), 0.0F);
    gtk_label_set_yalign(GTK_LABEL(state->stats_label), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(state->stats_label), TRUE);
    gtk_box_append(GTK_BOX(root), state->stats_label);

    gtk_box_append(GTK_BOX(root), make_heading("Recent bytes (hexadecimal)"));
    state->hex_view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(state->hex_view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->hex_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(state->hex_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(state->hex_view), GTK_WRAP_NONE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->hex_view)),
                             "Waiting for byte stream…\n", -1);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), state->hex_view);
    gtk_box_append(GTK_BOX(root), scroll);

    open_device(state);
    g_timeout_add(75, poll_stream, state);
    gtk_window_present(GTK_WINDOW(state->window));
}
} // namespace

int main(int argc, char **argv)
{
    AppState state;
    if (const char *env_device = std::getenv("STATICBUS_DEVICE"))
        state.device = env_device;

    // Consume our single optional --device PATH argument before GTK parses args.
    int out = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            state.device = argv[++i];
        } else if (std::strncmp(argv[i], "--device=", 9) == 0) {
            state.device = argv[i] + 9;
        } else {
            argv[out++] = argv[i];
        }
    }
    argc = out;
    argv[argc] = nullptr;

    GtkApplication *app = gtk_application_new("org.jordanlegare.staticbus",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state);
    const int result = g_application_run(G_APPLICATION(app), argc, argv);
    close_device(&state);
    g_object_unref(app);
    return result;
}
