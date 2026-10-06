// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_map_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace defragger {
namespace {

std::uint64_t number(const Json& data, const char* key)
{
    const auto* item = data.find(key);
    return item ? item->unsigned_or() : 0U;
}

const char* field(const Json& data, const char* key)
{
    const auto* item = data.find(key);
    return item && item->is_string() ? item->string().data() : "";
}

struct MapRasterCache {
    cairo_surface_t* surface = nullptr;
    int width = 0;
    int height = 0;
    int scale = 1;
    std::uint64_t generation = 0U;
};

void destroy_map_raster_cache(gpointer data)
{
    auto* cache = static_cast<MapRasterCache*>(data);
    if (cache == nullptr) return;
    if (cache->surface != nullptr)
        cairo_surface_destroy(cache->surface);
    delete cache;
}

std::uint32_t mix_rgb(
    std::uint32_t first, std::uint32_t second, double ratio)
{
    ratio = std::clamp(ratio, 0.0, 1.0);
    const auto channel = [ratio](
        std::uint32_t a, std::uint32_t b, unsigned shift) {
        const double av = static_cast<double>((a >> shift) & 0xffU);
        const double bv = static_cast<double>((b >> shift) & 0xffU);
        return static_cast<std::uint32_t>(
            std::clamp(av * (1.0 - ratio) + bv * ratio, 0.0, 255.0) + 0.5);
    };
    return (channel(first, second, 16U) << 16U) |
           (channel(first, second, 8U) << 8U) |
           channel(first, second, 0U);
}

std::uint32_t map_cell_rgb(const Json& cell)
{
    constexpr std::uint32_t free_colour = UINT32_C(0x050D1B);
    constexpr std::uint32_t outside_colour = UINT32_C(0x020408);
    constexpr std::uint32_t used_colour = UINT32_C(0x0585FF);
    constexpr std::uint32_t fragmented_colour = UINT32_C(0xFF253C);
    constexpr std::uint32_t directory_colour = UINT32_C(0x9E2BFA);
    constexpr std::uint32_t unknown_colour = UINT32_C(0x495468);
    constexpr std::uint32_t metadata_colour = UINT32_C(0xFF9F0A);

    const std::uint64_t free = number(cell, "free");
    const std::uint64_t outside = number(cell, "outside");
    const std::uint64_t used = number(cell, "used");
    const std::uint64_t free_like = free + outside;
    const std::uint64_t known = std::max<std::uint64_t>(1U, free_like + used);
    std::uint32_t colour = mix_rgb(
        free_colour, outside_colour,
        free_like == 0U
            ? 0.0
            : static_cast<double>(outside) / static_cast<double>(free_like));
    colour = mix_rgb(
        colour, used_colour,
        static_cast<double>(used) / static_cast<double>(known));

    const struct Overlay {
        const char* key;
        std::uint32_t colour;
        double minimum;
    } overlays[] = {
        {"directory", directory_colour, 0.58},
        {"fragmented", fragmented_colour, 0.70},
        {"bad", metadata_colour, 0.58},
    };
    for (const auto& overlay : overlays) {
        const std::uint64_t amount = number(cell, overlay.key);
        if (amount == 0U) continue;
        const double ratio = std::max(
            overlay.minimum,
            std::min(
                1.0,
                std::sqrt(
                    static_cast<double>(amount) /
                    static_cast<double>(known))));
        colour = mix_rgb(colour, overlay.colour, ratio);
    }

    const std::uint64_t unknown = number(cell, "unknown");
    const std::uint64_t total = free + outside + used + unknown;
    if (unknown != 0U && total != 0U)
        colour = mix_rgb(
            colour, unknown_colour,
            static_cast<double>(unknown) / static_cast<double>(total));
    return colour;
}

} // namespace

DesktopMapDisplayGeometry desktop_map_display_geometry(
    const Json& map, const std::vector<Json>& cells)
{
    DesktopMapDisplayGeometry geometry;
    if (cells.empty()) return geometry;

    geometry.first_unit = number(cells.front(), "start");
    geometry.last_unit = number(cells.back(), "end");
    if (geometry.last_unit < geometry.first_unit) return geometry;
    geometry.allocation_units =
        geometry.last_unit - geometry.first_unit + 1U;
    geometry.total_display_units = geometry.allocation_units;

    const std::uint64_t per_unit =
        number(map, "display_units_per_allocation_unit");
    const std::uint64_t prefix = number(map, "display_prefix_units");
    const std::uint64_t suffix = number(map, "display_suffix_units");
    const std::uint64_t display_total = number(map, "display_total_units");
    const std::uint64_t display_size = number(map, "display_unit_size");
    const char* display_name = field(map, "display_unit_name");

    bool one_cell_per_allocation_unit =
        static_cast<std::uint64_t>(cells.size()) == geometry.allocation_units;
    if (one_cell_per_allocation_unit) {
        std::uint64_t expected = geometry.first_unit;
        for (const auto& cell : cells) {
            const std::uint64_t start = number(cell, "start");
            const std::uint64_t end = number(cell, "end");
            if (start != expected || end != start) {
                one_cell_per_allocation_unit = false;
                break;
            }
            ++expected;
        }
    }

    if (!one_cell_per_allocation_unit || per_unit == 0U ||
        display_total == 0U || display_size == 0U ||
        display_name == nullptr || *display_name == '\0' ||
        geometry.allocation_units >
            std::numeric_limits<std::uint64_t>::max() / per_unit) {
        return geometry;
    }

    const std::uint64_t data_display_units =
        geometry.allocation_units * per_unit;
    if (prefix >
        std::numeric_limits<std::uint64_t>::max() - data_display_units) {
        return geometry;
    }
    const std::uint64_t prefix_and_data = prefix + data_display_units;
    if (suffix >
            std::numeric_limits<std::uint64_t>::max() - prefix_and_data ||
        prefix_and_data + suffix != display_total) {
        return geometry;
    }

    geometry.display_units_per_allocation_unit = per_unit;
    geometry.prefix_display_units = prefix;
    geometry.suffix_display_units = suffix;
    geometry.total_display_units = display_total;
    geometry.display_unit_size = display_size;
    geometry.display_unit_name = display_name;
    geometry.exact_subunits = true;
    return geometry;
}

gboolean desktop_draw_map(
    GtkWidget* widget,
    cairo_t* cr,
    const Json& map,
    const std::vector<Json>& cells,
    std::uint64_t generation)
{
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const int logical_width = std::max(1, allocation.width);
    const int logical_height = std::max(1, allocation.height);
    const int scale = std::max(1, gtk_widget_get_scale_factor(widget));
    if (logical_width > std::numeric_limits<int>::max() / scale ||
        logical_height > std::numeric_limits<int>::max() / scale) {
        return FALSE;
    }
    const int width = logical_width * scale;
    const int height = logical_height * scale;
    constexpr std::uint32_t background = UINT32_C(0x03050A);
    constexpr std::uint32_t metadata_colour = UINT32_C(0xFF9F0A);

    cairo_set_source_rgb(cr, 3.0 / 255.0, 5.0 / 255.0, 10.0 / 255.0);
    cairo_paint(cr);
    if (cells.empty()) return FALSE;

    const std::size_t source_count = cells.size();
    const std::size_t display_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    auto* cache = static_cast<MapRasterCache*>(
        g_object_get_data(G_OBJECT(widget), "defrag-map-raster-cache"));
    if (cache != nullptr && cache->surface != nullptr &&
        cache->width == width && cache->height == height &&
        cache->scale == scale && cache->generation == generation) {
        cairo_set_source_surface(cr, cache->surface, 0.0, 0.0);
        cairo_paint(cr);
        return FALSE;
    }
    if (cache == nullptr) {
        cache = new MapRasterCache();
        g_object_set_data_full(
            G_OBJECT(widget), "defrag-map-raster-cache", cache,
            destroy_map_raster_cache);
    }
    if (cache->surface != nullptr) {
        cairo_surface_destroy(cache->surface);
        cache->surface = nullptr;
    }
    cache->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(cache->surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(cache->surface);
        cache->surface = nullptr;
        return FALSE;
    }
    auto* pixels = reinterpret_cast<std::uint32_t*>(
        cairo_image_surface_get_data(cache->surface));
    std::fill_n(
        pixels, display_count,
        UINT32_C(0xFF000000) | background);

    const DesktopMapDisplayGeometry geometry =
        desktop_map_display_geometry(map, cells);
    if (geometry.exact_subunits) {
        const std::uint64_t total = geometry.total_display_units;
        const std::uint64_t data_start = geometry.prefix_display_units;
        const std::uint64_t data_end = total - geometry.suffix_display_units;
        const std::uint64_t per_allocation =
            geometry.display_units_per_allocation_unit;
        const std::uint64_t output_count =
            static_cast<std::uint64_t>(display_count);

        const auto colour_for_unit =
            [&cells, data_start, data_end, per_allocation,
             source_count](std::uint64_t unit) {
                if (unit < data_start || unit >= data_end)
                    return metadata_colour;
                const std::uint64_t local = unit - data_start;
                const std::uint64_t cell_index = local / per_allocation;
                if (cell_index >= static_cast<std::uint64_t>(source_count))
                    return background;
                return map_cell_rgb(
                    cells[static_cast<std::size_t>(cell_index)]);
            };

        if (total < output_count) {
            for (std::uint64_t pixel = 0U; pixel < output_count; ++pixel) {
                const std::uint64_t unit = (pixel * total) / output_count;
                pixels[static_cast<std::size_t>(pixel)] =
                    UINT32_C(0xFF000000) | colour_for_unit(unit);
            }
        } else {
            const std::uint64_t quotient = total / output_count;
            const std::uint64_t remainder = total % output_count;
            const auto boundary =
                [quotient, remainder, output_count](std::uint64_t index) {
                    return index * quotient +
                        (index * remainder) / output_count;
                };
            const auto overlap = [](
                std::uint64_t first_start,
                std::uint64_t first_end,
                std::uint64_t second_start,
                std::uint64_t second_end) {
                    const std::uint64_t start =
                        std::max(first_start, second_start);
                    const std::uint64_t end =
                        std::min(first_end, second_end);
                    return end > start ? end - start : 0U;
                };

            for (std::uint64_t pixel = 0U; pixel < output_count; ++pixel) {
                const std::uint64_t begin = boundary(pixel);
                const std::uint64_t end = boundary(pixel + 1U);
                double red = 0.0;
                double green = 0.0;
                double blue = 0.0;
                double weight = 0.0;
                const auto add_colour =
                    [&red, &green, &blue, &weight](
                        std::uint32_t rgb, std::uint64_t amount) {
                        if (amount == 0U) return;
                        const double w = static_cast<double>(amount);
                        red += static_cast<double>((rgb >> 16U) & 0xffU) * w;
                        green += static_cast<double>((rgb >> 8U) & 0xffU) * w;
                        blue += static_cast<double>(rgb & 0xffU) * w;
                        weight += w;
                    };

                add_colour(
                    metadata_colour,
                    overlap(begin, end, 0U, data_start));

                const std::uint64_t physical_data_begin =
                    std::max(begin, data_start);
                const std::uint64_t physical_data_end =
                    std::min(end, data_end);
                if (physical_data_end > physical_data_begin) {
                    std::uint64_t cursor = physical_data_begin - data_start;
                    const std::uint64_t local_end =
                        physical_data_end - data_start;
                    while (cursor < local_end) {
                        const std::uint64_t cell_index = cursor / per_allocation;
                        if (cell_index >=
                            static_cast<std::uint64_t>(source_count)) {
                            break;
                        }
                        const std::uint64_t next = std::min(
                            local_end,
                            (cell_index + 1U) * per_allocation);
                        add_colour(
                            map_cell_rgb(
                                cells[static_cast<std::size_t>(cell_index)]),
                            next - cursor);
                        cursor = next;
                    }
                }

                add_colour(
                    metadata_colour,
                    overlap(begin, end, data_end, total));

                if (weight != 0.0) {
                    const auto channel = [weight](double value) {
                        return static_cast<std::uint32_t>(
                            std::clamp(
                                std::lround(value / weight),
                                0L, 255L));
                    };
                    pixels[static_cast<std::size_t>(pixel)] =
                        UINT32_C(0xFF000000) |
                        (channel(red) << 16U) |
                        (channel(green) << 8U) |
                        channel(blue);
                }
            }
        }
    } else if (source_count < display_count) {
        for (std::size_t pixel = 0U; pixel < display_count; ++pixel) {
            const std::size_t source =
                pixel * source_count / display_count;
            pixels[pixel] =
                UINT32_C(0xFF000000) | map_cell_rgb(cells[source]);
        }
    } else {
        for (std::size_t pixel = 0U; pixel < display_count; ++pixel) {
            const std::size_t begin =
                pixel * source_count / display_count;
            const std::size_t end = std::max<std::size_t>(
                begin + 1U,
                (pixel + 1U) * source_count / display_count);

            if (end == begin + 1U) {
                pixels[pixel] =
                    UINT32_C(0xFF000000) | map_cell_rgb(cells[begin]);
                continue;
            }

            Json::Object combined;
            combined["start"] =
                Json::unsigned_integer(number(cells[begin], "start"));
            combined["end"] =
                Json::unsigned_integer(number(cells[end - 1U], "end"));
            for (const auto* key : {
                     "free", "outside", "used", "unknown",
                     "fragmented", "directory", "bad"}) {
                std::uint64_t total_value = 0U;
                for (std::size_t index = begin; index < end; ++index)
                    total_value += number(cells[index], key);
                combined[key] = Json::unsigned_integer(total_value);
            }
            pixels[pixel] =
                UINT32_C(0xFF000000) |
                map_cell_rgb(Json(std::move(combined)));
        }
    }

    cairo_surface_mark_dirty(cache->surface);
    cairo_surface_set_device_scale(
        cache->surface,
        static_cast<double>(scale),
        static_cast<double>(scale));
    cache->width = width;
    cache->height = height;
    cache->scale = scale;
    cache->generation = generation;
    cairo_set_source_surface(cr, cache->surface, 0.0, 0.0);
    cairo_paint(cr);
    return FALSE;
}

} // namespace defragger
