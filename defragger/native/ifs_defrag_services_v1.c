/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One typed, format-neutral table maps the application's verified host
 * services into every independent filesystem worker. */
#include "ifs_defrag_services_v1.h"
#include "ld_io.h"
#include "ld_device.h"
#include "ld_path.h"
#include "ld_runtime.h"
#include "ld_protocol.h"
#include "ld_stop.h"
const IfsDefragServicesV1 *ifs_defrag_services_v1(void) {
 static const IfsDefragServicesV1 services={
  .abi_version=IFS_DEFRAG_SERVICES_V1,
  .structure_bytes=sizeof(IfsDefragServicesV1),
  .ld_pread_full=ld_pread_full,
  .ld_pwrite_full=ld_pwrite_full,
  .ld_bitmap_size=ld_bitmap_size,
  .ld_bitmap_calloc=ld_bitmap_calloc,
  .ld_bitmap_get=ld_bitmap_get,
  .ld_bitmap_set=ld_bitmap_set,
  .ld_sync_fd=ld_sync_fd,
  .ld_fd_size_bytes=ld_fd_size_bytes,
  .ld_device_capture_binding=ld_device_capture_binding,
  .ld_device_open_verified_fd=ld_device_open_verified_fd,
  .ld_path_is_mounted=ld_path_is_mounted,
  .ld_path_parent_directory=ld_path_parent_directory,
  .ld_path_append_suffix=ld_path_append_suffix,
  .ld_path_is_derived_from=ld_path_is_derived_from,
  .ld_path_ensure_trusted_directory_tree=ld_path_ensure_trusted_directory_tree,
  .ld_path_require_staging_capacity=ld_path_require_staging_capacity,
  .ld_xstrdup=ld_xstrdup,
  .ld_emit_result_event=ld_emit_result_event,
  .ld_stop_requested=ld_stop_requested,
  .ld_stop_clear=ld_stop_clear,
  .ld_stop_install_handlers=ld_stop_install_handlers,
  .ld_stop_report_ready=ld_stop_report_ready,
  .ld_die=ld_die,
  .ld_xstrndup=ld_xstrndup,
  .ld_device_number_is_mounted=ld_device_number_is_mounted,
  .ld_fd_matches_identity=ld_fd_matches_identity,
  .ld_device_open=ld_device_open,
  .ld_device_close=ld_device_close,
  .ld_xcalloc=ld_xcalloc,
  .ld_device_try_open=ld_device_try_open,
  .ld_device_format_identity=ld_device_format_identity,
  .ld_device_matches_identity=ld_device_matches_identity,
 };
 return &services;
}
