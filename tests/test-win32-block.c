/* SPDX-License-Identifier: GPL-2.0-only */
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "block/block_int.h"
#include "block/win32-file.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "hw/block/block.h"
#include "sysemu/block-backend.h"

static DWORD write_error;
static unsigned int expected_alignment;
static int alignment_mode;
static HANDLE queried_handle;
static OVERLAPPED *packets[2];
static DWORD packet_errors[2];
static unsigned int packet_index;
static bool mock_queue;
static int filesystem_mode;
static volatile LONG flush_calls;
static DWORD flush_error;
static unsigned int flush_to_disk_calls;

static BOOL WINAPI test_volume_information(HANDLE file, WCHAR *volume,
    DWORD volume_size, DWORD *serial, DWORD *component_size, DWORD *flags,
    WCHAR *filesystem, DWORD filesystem_size)
{
    if (!filesystem_mode) {
        return GetVolumeInformationByHandleW(file, volume, volume_size, serial,
            component_size, flags, filesystem, filesystem_size);
    }
    if (filesystem_mode == 3) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    g_assert_cmpuint(filesystem_size, >=, 5);
    wcscpy(filesystem, filesystem_mode == 1 ? L"NTFS" : L"ReFS");
    return TRUE;
}

static BOOL WINAPI test_flush_file(HANDLE file)
{
    InterlockedIncrement(&flush_calls);
    if (flush_error) {
        SetLastError(flush_error);
        return FALSE;
    }
    return FlushFileBuffers(file);
}

static BOOL WINAPI test_write_file(HANDLE file, const void *buffer, DWORD bytes,
                                  DWORD *transferred, OVERLAPPED *ov)
{
    if (write_error) {
        if (expected_alignment) {
            g_assert_cmpuint((uintptr_t)buffer % expected_alignment, ==, 0);
        }
        SetLastError(write_error);
        return FALSE;
    }
    return WriteFile(file, buffer, bytes, transferred, ov);
}

static BOOL WINAPI test_get_completion(HANDLE port, DWORD *bytes,
                                      ULONG_PTR *key, OVERLAPPED **ov,
                                      DWORD timeout)
{
    if (!mock_queue) {
        return GetQueuedCompletionStatus(port, bytes, key, ov, timeout);
    }
    if (packet_index == G_N_ELEMENTS(packets)) {
        *ov = NULL;
        SetLastError(WAIT_TIMEOUT);
        return FALSE;
    }
    *ov = packets[packet_index];
    *bytes = packet_errors[packet_index] ? 0 : 512;
    *key = 0;
    SetLastError(packet_errors[packet_index]);
    return packet_errors[packet_index++] == ERROR_SUCCESS;
}

static BOOL WINAPI test_file_information(HANDLE file,
    FILE_INFO_BY_HANDLE_CLASS info_class, void *buffer, DWORD size)
{
    if (!alignment_mode) {
        return GetFileInformationByHandleEx(file, info_class, buffer, size);
    }
    queried_handle = file;
    if (alignment_mode == 3) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (info_class == WIN32_FILE_STORAGE_INFO) {
        Win32FileStorageInfo *storage = buffer;
        memset(storage, 0, sizeof(*storage));
        storage->logical_sector = alignment_mode == 2 ? 0 : 4096;
        storage->physical_atomicity = 4096;
    } else {
        g_assert_cmpint(info_class, ==, WIN32_FILE_ALIGNMENT_INFO);
        *(ULONG *)buffer = 4095;
    }
    return TRUE;
}

/* Exercise the backend implementation, with failures injected at Win32 APIs. */
#define WriteFile test_write_file
#define GetQueuedCompletionStatus test_get_completion
#define GetFileInformationByHandleEx test_file_information
#define GetVolumeInformationByHandleW test_volume_information
#define FlushFileBuffers test_flush_file
#include "../block/win32-aio.c"
#include "../block/file-win32.c"
#undef WriteFile
#undef GetQueuedCompletionStatus
#undef GetFileInformationByHandleEx
#undef GetVolumeInformationByHandleW
#undef FlushFileBuffers

static int coroutine_fn counted_flush_to_disk(BlockDriverState *bs)
{
    flush_to_disk_calls++;
    return raw_co_flush_to_disk(bs);
}

typedef struct Completion {
    int calls;
    int result;
} Completion;

static void completed(void *opaque, int result)
{
    Completion *completion = opaque;
    completion->calls++;
    completion->result = result;
}

static QEMUWin32AIOCB *request(BlockDriverState *bs, QEMUWin32AIOState *aio,
                              QEMUIOVector *qiov, Completion *completion,
                              bool read, bool bounce)
{
    QEMUWin32AIOCB *acb = qemu_aio_get(&win32_aiocb_info, bs,
                                     completed, completion);
    acb->ctx = aio;
    acb->qiov = qiov;
    acb->nbytes = qiov->size;
    acb->is_read = read;
    acb->is_linear = !bounce;
    if (bounce) {
        acb->buf = qemu_blockalign(bs, qiov->size);
        memset(acb->buf, 0xa7, qiov->size);
    }
    aio->count++;
    return acb;
}

static void test_failed_completion(void)
{
    BlockDriverState bs = {0};
    QEMUWin32AIOState *aio = win32_aio_init();
    char data[512];
    struct iovec iov = {data, sizeof(data)};
    QEMUIOVector qiov;
    Completion result[2] = {{0}};
    unsigned int i;

    g_assert_nonnull(aio);
    qemu_iovec_init_external(&qiov, &iov, 1);
    for (i = 0; i < G_N_ELEMENTS(packets); i++) {
        packets[i] = &request(&bs, aio, &qiov, &result[i], false, false)->ov;
    }
    packet_errors[0] = ERROR_DISK_FULL;
    packet_errors[1] = ERROR_SUCCESS;
    packet_index = 0;
    mock_queue = true;
    win32_aio_completion_cb(&aio->e);
    mock_queue = false;
    g_assert_cmpint(result[0].calls, ==, 1);
    g_assert_cmpint(result[0].result, ==, -ENOSPC);
    g_assert_cmpint(result[1].calls, ==, 1);
    g_assert_cmpint(result[1].result, ==, 0);
    g_assert_cmpint(aio->count, ==, 0);
    win32_aio_cleanup(aio);
}

static void test_short_read(void)
{
    BlockDriverState bs = {0};
    QEMUWin32AIOState aio = {0};
    unsigned char data[1024];
    struct iovec iov[] = {{data, 512}, {data + 512, 512}};
    QEMUIOVector qiov;
    Completion result = {0};
    QEMUWin32AIOCB *acb;
    unsigned int i;

    qemu_iovec_init_external(&qiov, iov, 2);
    memset(data, 0xcc, sizeof(data));
    acb = request(&bs, &aio, &qiov, &result, true, true);
    win32_aio_process_completion(&aio, acb, 600, ERROR_SUCCESS);
    for (i = 0; i < sizeof(data); i++) {
        g_assert_cmpuint(data[i], ==, i < 600 ? 0xa7 : 0);
    }
    g_assert_cmpint(result.result, ==, 0);
    acb = request(&bs, &aio, &qiov, &result, true, true);
    win32_aio_process_completion(&aio, acb, 0, ERROR_HANDLE_EOF);
    for (i = 0; i < sizeof(data); i++) {
        g_assert_cmpuint(data[i], ==, 0);
    }
    g_assert_cmpint(result.calls, ==, 2);
    g_assert_cmpint(aio.count, ==, 0);
}

static void test_submit_disk_full(void)
{
    BlockDriverState bs = {0};
    BlockDriver driver = {0};
    QEMUWin32AIOState *aio = win32_aio_init();
    void *buffer = qemu_memalign(4096, 8192);
    struct iovec iov = {(char *)buffer + 512, 4096};
    QEMUIOVector qiov;
    Completion result = {0};
    DWORD errors[] = {ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL};
    unsigned int i;

    g_assert_nonnull(aio);
    bs.drv = &driver;
    bs.aio_context = qemu_get_aio_context();
    bs.bl.min_mem_alignment = bs.bl.opt_mem_alignment = 4096;
    qemu_iovec_init_external(&qiov, &iov, 1);
    expected_alignment = 4096;
    for (i = 0; i < G_N_ELEMENTS(errors); i++) {
        write_error = errors[i];
        g_assert_nonnull(win32_aio_submit(&bs, aio, INVALID_HANDLE_VALUE, 0,
                                          &qiov, 8, completed, &result,
                                          QEMU_AIO_WRITE));
        g_assert_cmpint(result.calls, ==, i);
        g_assert_cmpint(aio->count, ==, 1);
        aio_poll(bs.aio_context, false);
        g_assert_cmpint(result.calls, ==, i + 1);
        g_assert_cmpint(result.result, ==, -ENOSPC);
        g_assert_cmpint(aio->count, ==, 0);
    }
    expected_alignment = write_error = 0;
    qemu_vfree(buffer);
    win32_aio_cleanup(aio);
}

static void test_threaded_disk_full(void)
{
    char data[512];
    struct iovec iov = {data, sizeof(data)};
    RawWin32AIOData *acb = g_new0(RawWin32AIOData, 1);
    acb->aio_iov = &iov;
    acb->aio_niov = 1;
    acb->aio_nbytes = sizeof(data);
    acb->aio_type = QEMU_AIO_WRITE;
    write_error = ERROR_DISK_FULL;
    g_assert_cmpint(aio_worker(acb), ==, -ENOSPC);
    write_error = 0;
}

static void test_alignment(void)
{
    BlockDriverState bs = {0};
    BDRVRawState state = {0};
    Error *err = NULL;
    bs.opaque = &state;
    bs.open_flags = BDRV_O_NOCACHE;
    state.hfile = (HANDLE)(uintptr_t)123;
    strcpy(state.drive_path, "Z:\\");
    alignment_mode = 1;
    raw_probe_alignment(&bs, &err);
    g_assert_null(err);
    g_assert_true(queried_handle == state.hfile);
    g_assert_cmpuint(bs.bl.request_alignment, ==, 4096);
    g_assert_cmpuint(bs.bl.min_mem_alignment, ==, 4096);
    g_assert_cmpuint(bs.bl.opt_mem_alignment, >=, 4096);
    for (alignment_mode = 2; alignment_mode <= 3; alignment_mode++) {
        raw_probe_alignment(&bs, &err);
        g_assert_nonnull(err);
        error_free(err);
        err = NULL;
    }
    alignment_mode = 0;
}

static void test_host_device_option(void)
{
    BlockDriverState bs = {0};
    BDRVRawState state = {0};
    QDict *options = qdict_new();
    Error *err = NULL;
    bs.opaque = &state;
    qdict_put_str(options, "filename", "unused");
    qdict_put_bool(options, "write-through", true);
    g_assert_cmpint(hdev_open(&bs, options, BDRV_O_RDWR, &err), ==, -EINVAL);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "write-through"));
    error_free(err);
    QDECREF(options);
}

static void test_file_alignment(void)
{
    BlockDriverState bs = {0};
    BDRVRawState state = {0};
    Error *err = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("kemu-alignment-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    state.hfile = (HANDLE)_get_osfhandle(fd);
    bs.opaque = &state;
    bs.open_flags = BDRV_O_NOCACHE;
    raw_probe_alignment(&bs, &err);
    _close(fd);
    g_assert_cmpint(unlink(path), ==, 0);
    g_free(path);
    if (err) {
        g_error("%s", error_get_pretty(err));
    }
    g_assert_cmpuint(bs.bl.request_alignment, >=, 512);
    g_assert_cmpuint(bs.bl.min_mem_alignment, >=, bs.bl.request_alignment);
    g_assert_cmpuint(bs.bl.opt_mem_alignment, >=, bs.bl.min_mem_alignment);
}

static void test_write_through_backend(gconstpointer opaque)
{
    bool durable = GPOINTER_TO_INT(opaque) == 1;
    BlockBackend *blk;
    BlockConf conf = {0};
    BlockDriverInfo info;
    QDict *options = qdict_new();
    char *path = NULL;
    int fd = g_file_open_tmp("kemu-write-through-XXXXXX", &path, NULL);
    void *buffer;
    LONG before;
    unsigned int before_flush_to_disk;
    int i;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(_chsize_s(fd, 16384), ==, 0);
    _close(fd);
    filesystem_mode = GPOINTER_TO_INT(opaque) == 4 ? 1 : GPOINTER_TO_INT(opaque);
    flush_calls = 0;
    qdict_put_str(options, "driver", "raw");
    qdict_put_str(options, "file.driver", "file");
    qdict_put_str(options, "file.aio", "native");
    qdict_put_bool(options, "file.write-through", GPOINTER_TO_INT(opaque) != 4);
    blk = blk_new_open(path, NULL, options, BDRV_O_RDWR | BDRV_O_NOCACHE,
                       &error_abort);
    g_assert_nonnull(blk);
    bdrv_file.bdrv_co_flush_to_disk = counted_flush_to_disk;
    g_assert_cmpint(flush_calls, ==, durable ? 1 : 0);
    g_assert_cmpint(bdrv_get_info(blk_bs(blk), &info), ==, 0);
    g_assert_cmpint(info.write_through, ==, durable);
    g_assert_cmpuint(blk_bs(blk)->supported_write_flags & BDRV_REQ_FUA,
                     ==, durable ? BDRV_REQ_FUA : 0);

    conf.blk = blk;
    conf.wce = ON_OFF_AUTO_AUTO;
    conf.rerror = conf.werror = BLOCKDEV_ON_ERROR_AUTO;
    g_assert_true(blkconf_apply_backend_options(&conf, false, true, &error_abort));
    g_assert_cmpint(blk_enable_write_cache(blk), ==, !durable);
    buffer = qemu_blockalign(blk_bs(blk), 4096);
    memset(buffer, 0xa7, 4096);

    /* Ordinary writes must not cause flushes in either cache policy. */
    before = flush_calls;
    before_flush_to_disk = flush_to_disk_calls;
    for (i = 0; i < 3; i++) {
        g_assert_cmpint(blk_pwrite(blk, i * 4096, buffer, 4096, 0), ==, 4096);
    }
    g_assert_cmpint(flush_calls, ==, before);
    g_assert_cmpuint(flush_to_disk_calls, ==, before_flush_to_disk);
    g_assert_cmpint(blk_flush(blk), ==, 0);
    g_assert_cmpint(flush_calls, ==, before + !durable);

    /* Native FUA bypasses emulation; fallback FUA still flushes. */
    before = flush_calls;
    before_flush_to_disk = flush_to_disk_calls;
    g_assert_cmpint(blk_pwrite(blk, 0, buffer, 4096, BDRV_REQ_FUA), ==, 4096);
    g_assert_cmpint(flush_calls, ==, before + !durable);
    g_assert_cmpuint(flush_to_disk_calls, ==, before_flush_to_disk + !durable);

    /* Explicit device cache settings take precedence over detection. */
    conf.wce = ON_OFF_AUTO_ON;
    g_assert_true(blkconf_apply_backend_options(&conf, false, true, &error_abort));
    g_assert_true(blk_enable_write_cache(blk));
    conf.wce = ON_OFF_AUTO_OFF;
    g_assert_true(blkconf_apply_backend_options(&conf, false, true, &error_abort));
    g_assert_false(blk_enable_write_cache(blk));
    before = flush_calls;
    g_assert_cmpint(blk_pwrite(blk, 0, buffer, 4096, 0), ==, 4096);
    g_assert_cmpint(flush_calls, ==, before + !durable);

    /* Failed writes do not become successful FUA completions. */
    before = flush_calls;
    write_error = ERROR_DISK_FULL;
    g_assert_cmpint(blk_pwrite(blk, 0, buffer, 4096, BDRV_REQ_FUA), ==, -ENOSPC);
    write_error = 0;
    g_assert_cmpint(flush_calls, ==, before);
    if (!durable) {
        flush_error = ERROR_ACCESS_DENIED;
        g_assert_cmpint(blk_pwrite(blk, 0, buffer, 4096, BDRV_REQ_FUA), ==, -EIO);
        flush_error = 0;
    }

    qemu_vfree(buffer);
    blk_unref(blk);
    bdrv_file.bdrv_co_flush_to_disk = raw_co_flush_to_disk;
    filesystem_mode = 0;
    g_assert_cmpint(unlink(path), ==, 0);
    g_free(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    module_call_init(MODULE_INIT_QOM);
    bdrv_init();
    g_test_add_func("/win32-block/failed-completion", test_failed_completion);
    g_test_add_func("/win32-block/short-read", test_short_read);
    g_test_add_func("/win32-block/submit-disk-full", test_submit_disk_full);
    g_test_add_func("/win32-block/threaded-disk-full", test_threaded_disk_full);
    g_test_add_func("/win32-block/alignment", test_alignment);
    g_test_add_func("/win32-block/host-device-option", test_host_device_option);
    g_test_add_func("/win32-block/file-alignment", test_file_alignment);
    g_test_add_data_func("/win32-block/write-through/ntfs", GINT_TO_POINTER(1),
                         test_write_through_backend);
    g_test_add_data_func("/win32-block/write-through/other-filesystem",
                         GINT_TO_POINTER(2), test_write_through_backend);
    g_test_add_data_func("/win32-block/write-through/query-failure",
                         GINT_TO_POINTER(3), test_write_through_backend);
    g_test_add_data_func("/win32-block/write-through/disabled", GINT_TO_POINTER(4),
                         test_write_through_backend);
    return g_test_run();
}
