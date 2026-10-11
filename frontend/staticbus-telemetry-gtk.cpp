// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Software-only staticbus telemetry frontend.
 *
 * Reads supported PMU counters via perf_event_open and interrupt totals from
 * /proc/stat. It does not sample physical DDR bus wires or recover raw bus data.
 */
#include <gtk/gtk.h>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

namespace {
constexpr const char *kPmuRoot = "/sys/bus/event_source/devices";
constexpr const char *kCpuOnline = "/sys/devices/system/cpu/online";

struct CounterFd {
    int fd{-1};
    std::uint64_t last_scaled{0};
    bool has_last{false};
    ~CounterFd() { if (fd >= 0) ::close(fd); }
    CounterFd() = default;
    CounterFd(const CounterFd&) = delete;
    CounterFd& operator=(const CounterFd&) = delete;
    CounterFd(CounterFd&& other) noexcept
        : fd(other.fd), last_scaled(other.last_scaled), has_last(other.has_last) {
        other.fd = -1;
    }
    CounterFd& operator=(CounterFd&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) ::close(fd);
            fd = other.fd;
            last_scaled = other.last_scaled;
            has_last = other.has_last;
            other.fd = -1;
        }
        return *this;
    }
};

struct Metric {
    std::string label;
    std::string source;
    std::string detail;
    std::vector<CounterFd> counters;
    double rate{0.0};
    double unit_scale{1.0};
    std::string display_unit{"events"};
    bool available{false};
    bool initialized{false};
};

struct App {
    GtkWidget *window{};
    GtkWidget *status{};
    GtkWidget *imc_read_label{};
    GtkWidget *imc_write_label{};
    GtkWidget *cache_label{};
    GtkWidget *irq_label{};
    GtkWidget *pt_label{};
    GtkWidget *edac_label{};
    Metric imc_reads{"IMC data_reads", "uncore_imc", "", {}, 0.0, 1.0, "events", false, false};
    Metric imc_writes{"IMC data_writes", "uncore_imc", "", {}, 0.0, 1.0, "events", false, false};
    Metric cache_misses{"CPU cache misses", "cpu", "", {}, 0.0, 1.0, "events", false, false};
    std::uint64_t last_interrupts{0};
    bool have_interrupts{false};
    gint64 last_tick_us{0};
};

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c); });
    if (first == value.end()) return {};
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                       [](unsigned char c) { return std::isspace(c); }).base();
    return std::string(first, last);
}

std::string read_text(const fs::path& path)
{
    std::ifstream file(path);
    if (!file) return {};
    std::ostringstream out;
    out << file.rdbuf();
    return trim(out.str());
}

bool parse_u64(const std::string& text, std::uint64_t& value)
{
    try {
        std::size_t used = 0;
        value = std::stoull(trim(text), &used, 0);
        return used == trim(text).size();
    } catch (...) {
        return false;
    }
}

std::vector<std::string> split(const std::string& input, char delimiter)
{
    std::vector<std::string> result;
    std::stringstream stream(input);
    std::string item;
    while (std::getline(stream, item, delimiter))
        result.push_back(trim(item));
    return result;
}

std::vector<int> parse_cpu_list(const std::string& list)
{
    std::vector<int> cpus;
    for (const auto& part : split(list, ',')) {
        if (part.empty()) continue;
        const auto dash = part.find('-');
        try {
            if (dash == std::string::npos) {
                cpus.push_back(std::stoi(part));
            } else {
                const int first = std::stoi(part.substr(0, dash));
                const int last = std::stoi(part.substr(dash + 1));
                if (last < first || last - first > 4096) continue;
                for (int cpu = first; cpu <= last; ++cpu) cpus.push_back(cpu);
            }
        } catch (...) {}
    }
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    return cpus;
}

std::vector<int> online_cpus()
{
    auto cpus = parse_cpu_list(read_text(kCpuOnline));
    if (cpus.empty()) cpus.push_back(0);
    return cpus;
}

int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                    int group_fd, unsigned long flags)
{
    return static_cast<int>(syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags));
}

std::vector<int> bit_positions(const std::string& description)
{
    std::vector<int> bits;
    for (const auto& range : split(description, ',')) {
        const auto dash = range.find('-');
        try {
            if (dash == std::string::npos) {
                bits.push_back(std::stoi(range));
            } else {
                const int first = std::stoi(range.substr(0, dash));
                const int last = std::stoi(range.substr(dash + 1));
                if (last < first || last - first > 63) return {};
                for (int bit = first; bit <= last; ++bit) bits.push_back(bit);
            }
        } catch (...) { return {}; }
    }
    return bits;
}

bool encode_format_field(const fs::path& format_path, std::uint64_t value,
                         perf_event_attr& attr)
{
    const std::string description = read_text(format_path);
    const auto colon = description.find(':');
    if (colon == std::string::npos) return false;
    const std::string reg = trim(description.substr(0, colon));
    const auto dest_bits = bit_positions(description.substr(colon + 1));
    if (dest_bits.empty() || dest_bits.size() > 64) return false;

    std::uint64_t encoded = 0;
    for (std::size_t bit = 0; bit < dest_bits.size(); ++bit) {
        if (dest_bits[bit] < 0 || dest_bits[bit] >= 64) return false;
        if ((value >> bit) & 1ULL) encoded |= (1ULL << dest_bits[bit]);
    }

    if (reg == "config") attr.config |= encoded;
    else if (reg == "config1") attr.config1 |= encoded;
    else if (reg == "config2") attr.config2 |= encoded;
    else return false;
    return true;
}

bool build_pmu_attr(const fs::path& pmu_path, const std::string& event_name,
                    perf_event_attr& attr, std::string& error)
{
    std::uint64_t type = 0;
    if (!parse_u64(read_text(pmu_path / "type"), type)) {
        error = "Cannot read PMU type";
        return false;
    }

    const std::string alias = read_text(pmu_path / "events" / event_name);
    if (alias.empty()) {
        error = "Event alias is not exported by this PMU";
        return false;
    }

    attr = {};
    attr.size = sizeof(attr);
    attr.type = static_cast<std::uint32_t>(type);
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    attr.disabled = 1;
    attr.exclude_guest = 1;

    for (const auto& item : split(alias, ',')) {
        const auto equals = item.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(item.substr(0, equals));
        const std::string raw_value = trim(item.substr(equals + 1));
        std::uint64_t value = 0;
        if (!parse_u64(raw_value, value)) {
            error = "Cannot parse event alias field " + key;
            return false;
        }
        if (!encode_format_field(pmu_path / "format" / key, value, attr)) {
            error = "Unsupported event format field " + key;
            return false;
        }
    }
    return true;
}

bool enable_event(perf_event_attr& attr, int cpu, CounterFd& counter, std::string& error)
{
    const int fd = perf_event_open(&attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (ioctl(fd, PERF_EVENT_IOC_RESET, 0) < 0 ||
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
        error = std::strerror(errno);
        ::close(fd);
        return false;
    }
    counter.fd = fd;
    return true;
}

std::vector<int> pmu_cpus(const fs::path& pmu_path)
{
    std::string cpus = read_text(pmu_path / "cpumask");
    if (cpus.empty()) cpus = read_text(pmu_path / "cpus");
    if (cpus.empty()) return {};
    return parse_cpu_list(cpus);
}

void open_uncore_metric(Metric& metric, const std::string& event_name)
{
    std::error_code ec;
    const fs::path root(kPmuRoot);
    if (!fs::exists(root, ec)) {
        metric.detail = "PMU sysfs is not available";
        return;
    }

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        const std::string pmu_name = entry.path().filename().string();
        if (pmu_name.rfind("uncore_imc", 0) != 0) continue;
        if (!fs::exists(entry.path() / "events" / event_name, ec)) continue;

        // Honour perf's own unit/scale metadata so the GUI can report MiB/s
        // when this PMU exports data_reads.scale/data_reads.unit.
        const std::string scale_text = read_text(entry.path() / "events" / (event_name + ".scale"));
        const std::string unit_text = read_text(entry.path() / "events" / (event_name + ".unit"));
        if (!scale_text.empty()) {
            try {
                const double scale = std::stod(scale_text);
                if (std::isfinite(scale) && scale > 0.0)
                    metric.unit_scale = scale;
            } catch (...) {
                metric.detail = pmu_name + ": invalid event scale; using raw counts";
            }
        }
        if (!unit_text.empty())
            metric.display_unit = unit_text;

        std::string error;
        perf_event_attr attr{};
        if (!build_pmu_attr(entry.path(), event_name, attr, error)) {
            metric.detail = pmu_name + ": " + error;
            continue;
        }
        const auto cpus = pmu_cpus(entry.path());
        if (cpus.empty()) {
            metric.detail = pmu_name + ": no PMU cpumask/cpus file";
            continue;
        }
        /*
         * Uncore PMU directories represent hardware instances. Open once on
         * the first representative CPU in that instance's cpumask; opening on
         * every CPU in the mask can double-count a shared uncore counter.
         */
        const int cpu = cpus.front();
        CounterFd counter;
        std::string open_error;
        if (enable_event(attr, cpu, counter, open_error)) {
            metric.counters.push_back(std::move(counter));
        } else if (metric.detail.empty()) {
            metric.detail = pmu_name + " CPU " + std::to_string(cpu) + ": " + open_error;
        }
    }

    metric.available = !metric.counters.empty();
    if (metric.available) {
        metric.detail = std::to_string(metric.counters.size()) +
                        " PMU instance counter(s) opened; unit/scale from sysfs when provided";
    } else if (metric.detail.empty()) {
        metric.detail = "Event unavailable or perf permissions denied";
    }
}

void open_core_cache_metric(Metric& metric)
{
    const auto cpus = online_cpus();
    for (int cpu : cpus) {
        perf_event_attr attr{};
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = PERF_COUNT_HW_CACHE_MISSES;
        attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        attr.disabled = 1;
        CounterFd counter;
        std::string error;
        if (enable_event(attr, cpu, counter, error))
            metric.counters.push_back(std::move(counter));
        else if (metric.detail.empty())
            metric.detail = "CPU " + std::to_string(cpu) + ": " + error;
    }
    metric.available = !metric.counters.empty();
    if (metric.available) {
        metric.detail = std::to_string(metric.counters.size()) + " CPU counter(s) opened";
        metric.display_unit = "events";
        metric.unit_scale = 1.0;
    }
    else if (metric.detail.empty())
        metric.detail = "Event unavailable or perf permissions denied";
}

std::uint64_t read_scaled_total(Metric& metric, bool& ok)
{
    ok = true;
    std::uint64_t total = 0;
    for (auto& counter : metric.counters) {
        struct {
            std::uint64_t value;
            std::uint64_t time_enabled;
            std::uint64_t time_running;
        } result{};
        const ssize_t n = ::read(counter.fd, &result, sizeof(result));
        if (n != static_cast<ssize_t>(sizeof(result))) {
            ok = false;
            continue;
        }
        std::uint64_t scaled = result.value;
        if (result.time_running && result.time_enabled > result.time_running) {
            const long double factor = static_cast<long double>(result.time_enabled) /
                                       static_cast<long double>(result.time_running);
            const long double adjusted = static_cast<long double>(scaled) * factor;
            if (adjusted < static_cast<long double>(UINT64_MAX))
                scaled = static_cast<std::uint64_t>(adjusted);
        }
        if (UINT64_MAX - total < scaled) total = UINT64_MAX;
        else total += scaled;
    }
    return total;
}

bool read_interrupt_total(std::uint64_t& value)
{
    std::ifstream file("/proc/stat");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("intr ", 0) != 0) continue;
        std::istringstream input(line.substr(5));
        return static_cast<bool>(input >> value);
    }
    return false;
}

std::string read_edac_summary()
{
    const fs::path root("/sys/devices/system/edac/mc");
    std::error_code ec;
    if (!fs::exists(root, ec)) return "EDAC: not exposed by this kernel/platform";
    std::uint64_t ce_total = 0, ue_total = 0;
    unsigned int controllers = 0;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("mc", 0) != 0 || name == "mc" ||
            !fs::is_directory(entry.path(), ec)) continue;
        std::uint64_t value = 0;
        if (parse_u64(read_text(entry.path() / "ce_count"), value)) ce_total += value;
        if (parse_u64(read_text(entry.path() / "ue_count"), value)) ue_total += value;
        ++controllers;
    }
    if (controllers == 0) return "EDAC: no active memory-controller counters found";
    return "EDAC controllers: " + std::to_string(controllers) +
           "   Correctable errors: " + std::to_string(ce_total) +
           "   Uncorrectable errors: " + std::to_string(ue_total);
}

void set_value(GtkWidget *label, const std::string& value)
{
    gtk_label_set_text(GTK_LABEL(label), value.c_str());
}

std::string metric_description(const Metric& metric, const std::string& fallback_unit)
{
    if (!metric.available) return "Unavailable: " + metric.detail;
    std::ostringstream value;
    value.setf(std::ios::fixed);
    value.precision(metric.display_unit == "MiB" || metric.display_unit == "MB" ? 2 : 0);
    value << metric.rate;
    const std::string unit = metric.display_unit.empty()
                           ? fallback_unit : metric.display_unit + "/s";
    return metric.label + ": " + value.str() + " " + unit +
           "\n" + metric.detail;
}

gboolean sample_metrics(gpointer data)
{
    auto *app = static_cast<App *>(data);
    const gint64 now = g_get_monotonic_time();
    double elapsed = 1.0;
    if (app->last_tick_us != 0)
        elapsed = std::max(0.001, static_cast<double>(now - app->last_tick_us) / 1'000'000.0);
    app->last_tick_us = now;


    // Per-metric sample baselines, kept independently from the displayed rate.
    static std::map<std::string, std::uint64_t> previous_counts;
    auto sample_with_baseline = [elapsed, &previous_counts](Metric& metric) {
        if (!metric.available) return;
        bool ok = false;
        const auto current = read_scaled_total(metric, ok);
        if (!ok) {
            metric.available = false;
            metric.detail = "Counter read failed; permissions or device state changed";
            return;
        }
        const auto it = previous_counts.find(metric.label);
        if (it != previous_counts.end()) {
            const std::uint64_t delta = current >= it->second ? current - it->second : 0;
            const long double per_second =
                (static_cast<long double>(delta) / elapsed) * metric.unit_scale;
            metric.rate = per_second > static_cast<long double>(UINT64_MAX)
                        ? static_cast<double>(UINT64_MAX) : static_cast<double>(per_second);
        } else {
            metric.rate = 0;
        }
        previous_counts[metric.label] = current;
    };

    sample_with_baseline(app->imc_reads);
    sample_with_baseline(app->imc_writes);
    sample_with_baseline(app->cache_misses);

    std::uint64_t interrupts = 0;
    if (read_interrupt_total(interrupts)) {
        if (app->have_interrupts && interrupts >= app->last_interrupts) {
            const auto delta = interrupts - app->last_interrupts;
            const auto per_second = static_cast<long double>(delta) / elapsed;
            set_value(app->irq_label, "Host interrupts: " +
                std::to_string(static_cast<std::uint64_t>(per_second)) + " interrupts/s");
        } else {
            set_value(app->irq_label, "Host interrupts: sampling baseline…");
        }
        app->last_interrupts = interrupts;
        app->have_interrupts = true;
    } else {
        set_value(app->irq_label, "Host interrupts: /proc/stat unavailable");
    }

    set_value(app->imc_read_label, metric_description(app->imc_reads, "events/s"));
    set_value(app->imc_write_label, metric_description(app->imc_writes, "events/s"));
    set_value(app->cache_label, metric_description(app->cache_misses, "events/s"));
    set_value(app->edac_label, read_edac_summary());

    const bool pt_available = fs::exists(fs::path(kPmuRoot) / "intel_pt");
    set_value(app->pt_label, pt_available
        ? "Intel PT: advertised by this CPU/kernel. Separate, explicit perf recordings capture execution flow—not DDR data bytes."
        : "Intel PT: no intel_pt PMU found.");

    std::string status = "Software-only telemetry. No external probes. ";
    status += (app->imc_reads.available || app->imc_writes.available)
        ? "Memory-controller PMU events are open."
        : "IMC counters unavailable: inspect permissions and PMU alias details.";
    set_value(app->status, status);
    return G_SOURCE_CONTINUE;
}

GtkWidget *make_metric(const char *title, GtkWidget **label_out)
{
    GtkWidget *frame = gtk_frame_new(title);
    GtkWidget *label = gtk_label_new("Probing available interfaces…");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_label_set_yalign(GTK_LABEL(label), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_set_margin_top(label, 10);
    gtk_widget_set_margin_bottom(label, 10);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_frame_set_child(GTK_FRAME(frame), label);
    *label_out = label;
    return frame;
}

void activate(GtkApplication *application, gpointer user_data)
{
    auto *app = static_cast<App *>(user_data);
    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "staticbus — Software Telemetry");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 900, 680);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_window_set_child(GTK_WINDOW(app->window), root);

    GtkWidget *title = gtk_label_new("staticbus / software telemetry");
    gtk_widget_add_css_class(title, "title-1");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), title);

    GtkWidget *intro = gtk_label_new(
        "Live no-probe telemetry from Linux perf PMUs, /proc/stat and EDAC sysfs. "
        "Counters are not raw DDR bus bytes.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
    gtk_box_append(GTK_BOX(root), intro);

    app->status = gtk_label_new("Discovering hardware-supported counters…");
    gtk_label_set_xalign(GTK_LABEL(app->status), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(app->status), TRUE);
    gtk_widget_add_css_class(app->status, "dim-label");
    gtk_box_append(GTK_BOX(root), app->status);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_vexpand(grid, TRUE);
    gtk_box_append(GTK_BOX(root), grid);
    gtk_grid_attach(GTK_GRID(grid), make_metric("Memory-controller reads", &app->imc_read_label), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_metric("Memory-controller writes", &app->imc_write_label), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_metric("CPU cache misses", &app->cache_label), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_metric("Interrupt activity", &app->irq_label), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_metric("Intel Processor Trace", &app->pt_label), 0, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), make_metric("Memory error reporting", &app->edac_label), 0, 3, 2, 1);

    std::error_code ec;
    if (fs::exists(fs::path(kPmuRoot) / "uncore_imc", ec)) {
        open_uncore_metric(app->imc_reads, "data_reads");
        open_uncore_metric(app->imc_writes, "data_writes");
    } else {
        app->imc_reads.detail = "No uncore_imc PMU exported";
        app->imc_writes.detail = "No uncore_imc PMU exported";
    }
    open_core_cache_metric(app->cache_misses);
    app->last_tick_us = g_get_monotonic_time();
    g_timeout_add(1000, sample_metrics, app);
    gtk_window_present(GTK_WINDOW(app->window));
}
} // namespace

int main(int argc, char **argv)
{
    App app;
    GtkApplication *application = gtk_application_new(
        "org.jordanlegare.staticbus.telemetry", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    const int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
