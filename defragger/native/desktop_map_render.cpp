// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_map_render.hpp"
#include "desktop_map_geometry.hpp"

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

gboolean desktop_draw_map(
    GtkWidget* widget,
    cairo_t* cr,
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

    const auto raster = desktop_map_raster_geometry(cells, display_count);
    if (raster.pixels == 0U) return FALSE;
    std::size_t previously_rendered_source = source_count;
    for (std::size_t pixel = 0U; pixel < raster.pixels; ++pixel) {
        const std::size_t begin =
            pixel * source_count / raster.pixels;
        // Restore the full-surface magnification of small volumes.
        // Copy the colour of the same genuine allocation cell for adjacent
        // pixels, rather than fabricating a sector or sub-cluster state.
        if (raster.enlarged && begin == previously_rendered_source) {
            pixels[pixel] = pixels[pixel - 1U];
            continue;
        }
        previously_rendered_source = begin;
        const std::size_t end = std::max<std::size_t>(
            begin + 1U,
            (pixel + 1U) * source_count / raster.pixels);

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
