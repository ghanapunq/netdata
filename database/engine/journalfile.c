// SPDX-License-Identifier: GPL-3.0-or-later
#include "rrdengine.h"


// DBENGINE2: Helper

static void update_metric_retention_and_granularity_by_uuid(
        struct rrdengine_instance *ctx, uuid_t *uuid,
        time_t first_time_s, time_t last_time_s,
        time_t update_every_s, time_t now_s)
{
    if(last_time_s > now_s) {
        error_limit_static_global_var(erl, 1, 0);
        error_limit(&erl, "DBENGINE JV2: wrong last time on-disk (%ld - %ld, now %ld), "
                          "fixing last time to now",
                    first_time_s, last_time_s, now_s);
        last_time_s = now_s;
    }

    if(first_time_s > last_time_s) {
        error_limit_static_global_var(erl, 1, 0);
        error_limit(&erl, "DBENGINE JV2: wrong first time on-disk (%ld - %ld, now %ld), "
                          "fixing first time to last time",
                    first_time_s, last_time_s, now_s);

        first_time_s = last_time_s;
    }

    if(first_time_s == 0 ||
            last_time_s == 0
        ) {
        error_limit_static_global_var(erl, 1, 0);
        error_limit(&erl, "DBENGINE JV2: zero on-disk timestamps (%ld - %ld, now %ld), "
                          "using them as-is",
                    first_time_s, last_time_s, now_s);
    }

    MRG_ENTRY entry = {
            .section = (Word_t)ctx,
            .first_time_s = first_time_s,
            .last_time_s = last_time_s,
            .latest_update_every_s = update_every_s
    };
    uuid_copy(entry.uuid, *uuid);

    bool added;
    METRIC *metric = mrg_metric_add_and_acquire(main_mrg, entry, &added);

    if (likely(!added))
        mrg_metric_expand_retention(main_mrg, metric, first_time_s, last_time_s, update_every_s);

    mrg_metric_release(main_mrg, metric);
}

static void flush_transaction_buffer_cb(uv_fs_t* req)
{
    worker_is_busy(RRDENG_FLUSH_TRANSACTION_BUFFER_CB);

    WAL *wal = req->data;
    struct generic_io_descriptor *io_descr = &wal->io_descr;
    struct rrdengine_instance *ctx = io_descr->ctx;

    debug(D_RRDENGINE, "%s: Journal block was written to disk.", __func__);
    if (req->result < 0) {
        ++ctx->stats.io_errors;
        rrd_stat_atomic_add(&global_io_errors, 1);
        error("DBENGINE: %s: uv_fs_write: %s", __func__, uv_strerror((int)req->result));
    } else {
        debug(D_RRDENGINE, "%s: Journal block was written to disk.", __func__);
    }

    uv_fs_req_cleanup(req);
    wal_release(wal);

    __atomic_sub_fetch(&ctx->worker_config.atomics.extents_currently_being_flushed, 1, __ATOMIC_RELAXED);

    worker_is_idle();
}

/* Careful to always call this before creating a new journal file */
void wal_flush_transaction_buffer(struct rrdengine_instance *ctx, struct rrdengine_datafile *datafile, WAL *wal, uv_loop_t *loop)
{
    int ret;
    struct generic_io_descriptor *io_descr;
    struct rrdengine_journalfile *journalfile = datafile->journalfile;

    io_descr = &wal->io_descr;
    io_descr->ctx = ctx;
    if (wal->size < wal->buf_size) {
        /* simulate an empty transaction to skip the rest of the block */
        *(uint8_t *) (wal->buf + wal->size) = STORE_PADDING;
    }
    io_descr->buf = wal->buf;
    io_descr->bytes = wal->buf_size;
    io_descr->pos = journalfile->pos;
    io_descr->req.data = wal;
    io_descr->data = journalfile;
    io_descr->completion = NULL;

    io_descr->iov = uv_buf_init((void *)io_descr->buf, wal->buf_size);
    ret = uv_fs_write(loop, &io_descr->req, journalfile->file, &io_descr->iov, 1,
                      journalfile->pos, flush_transaction_buffer_cb);
    fatal_assert(-1 != ret);
    journalfile->pos += wal->buf_size;
    ctx->disk_space += wal->buf_size;
    ctx->stats.io_write_bytes += wal->buf_size;
    ++ctx->stats.io_write_requests;
}

void generate_journalfilepath_v2(struct rrdengine_datafile *datafile, char *str, size_t maxlen)
{
    (void) snprintfz(str, maxlen, "%s/" WALFILE_PREFIX RRDENG_FILE_NUMBER_PRINT_TMPL WALFILE_EXTENSION_V2,
                    datafile->ctx->dbfiles_path, datafile->tier, datafile->fileno);
}

void generate_journalfilepath(struct rrdengine_datafile *datafile, char *str, size_t maxlen)
{
    (void) snprintfz(str, maxlen, "%s/" WALFILE_PREFIX RRDENG_FILE_NUMBER_PRINT_TMPL WALFILE_EXTENSION,
                    datafile->ctx->dbfiles_path, datafile->tier, datafile->fileno);
}

void journalfile_init(struct rrdengine_journalfile *journalfile, struct rrdengine_datafile *datafile)
{
    journalfile->file = (uv_file)0;
    journalfile->pos = 0;
    journalfile->datafile = datafile;
    SET_JOURNAL_DATA(journalfile, 0);
    SET_JOURNAL_DATA_SIZE(journalfile, 0);
    journalfile->data = NULL;
}

static int close_uv_file(struct rrdengine_datafile *datafile, uv_file file)
{
    int ret;
    char path[RRDENG_PATH_MAX];

    uv_fs_t req;
    ret = uv_fs_close(NULL, &req, file, NULL);
    if (ret < 0) {
    generate_journalfilepath(datafile, path, sizeof(path));
        error("DBENGINE: uv_fs_close(%s): %s", path, uv_strerror(ret));
        ++datafile->ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
    }
    uv_fs_req_cleanup(&req);
    return ret;
}

int close_journal_file(struct rrdengine_journalfile *journalfile, struct rrdengine_datafile *datafile)
{
    struct rrdengine_instance *ctx = datafile->ctx;
    char path[RRDENG_PATH_MAX];

    void *journal_data = GET_JOURNAL_DATA(journalfile);
    size_t journal_data_size = GET_JOURNAL_DATA_SIZE(journalfile);

    if (likely(journal_data)) {
        if (munmap(journal_data, journal_data_size)) {
            generate_journalfilepath_v2(datafile, path, sizeof(path));
            error("DBENGINE: failed to unmap journal index file for %s", path);
            ++ctx->stats.fs_errors;
            rrd_stat_atomic_add(&global_fs_errors, 1);
        }
        SET_JOURNAL_DATA(journalfile, 0);
        SET_JOURNAL_DATA_SIZE(journalfile, 0);
        return 0;
    }

    return close_uv_file(datafile, journalfile->file);
}

int unlink_journal_file(struct rrdengine_journalfile *journalfile)
{
    struct rrdengine_datafile *datafile = journalfile->datafile;
    struct rrdengine_instance *ctx = datafile->ctx;
    uv_fs_t req;
    int ret;
    char path[RRDENG_PATH_MAX];

    generate_journalfilepath(datafile, path, sizeof(path));

    ret = uv_fs_unlink(NULL, &req, path, NULL);
    if (ret < 0) {
        error("DBENGINE: uv_fs_fsunlink(%s): %s", path, uv_strerror(ret));
        ++ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
    }
    uv_fs_req_cleanup(&req);

    ++ctx->stats.journalfile_deletions;

    return ret;
}

int destroy_journal_file_unsafe(struct rrdengine_journalfile *journalfile, struct rrdengine_datafile *datafile)
{
    struct rrdengine_instance *ctx = datafile->ctx;
    uv_fs_t req;
    int ret;
    char path[RRDENG_PATH_MAX];
    char path_v2[RRDENG_PATH_MAX];

    generate_journalfilepath(datafile, path, sizeof(path));
    generate_journalfilepath_v2(datafile, path_v2, sizeof(path));

    if (journalfile->file) {
    ret = uv_fs_ftruncate(NULL, &req, journalfile->file, 0, NULL);
    if (ret < 0) {
        error("DBENGINE: uv_fs_ftruncate(%s): %s", path, uv_strerror(ret));
        ++ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
    }
    uv_fs_req_cleanup(&req);
        (void) close_uv_file(datafile, journalfile->file);
    }

    // This is the new journal v2 index file
    ret = uv_fs_unlink(NULL, &req, path_v2, NULL);
    if (ret < 0) {
        error("DBENGINE: uv_fs_fsunlink(%s): %s", path, uv_strerror(ret));
        ++ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
    }
    uv_fs_req_cleanup(&req);

    ret = uv_fs_unlink(NULL, &req, path, NULL);
    if (ret < 0) {
        error("DBENGINE: uv_fs_fsunlink(%s): %s", path, uv_strerror(ret));
        ++ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
    }
    uv_fs_req_cleanup(&req);

    ++ctx->stats.journalfile_deletions;
    ++ctx->stats.journalfile_deletions;

    void *journal_data = GET_JOURNAL_DATA(journalfile);
    size_t journal_data_size = GET_JOURNAL_DATA_SIZE(journalfile);

    if (journal_data) {
        if (munmap(journal_data, journal_data_size)) {
            error("DBENGINE: failed to unmap index file %s", path_v2);
        }
    }

    return ret;
}

int create_journal_file(struct rrdengine_journalfile *journalfile, struct rrdengine_datafile *datafile)
{
    struct rrdengine_instance *ctx = datafile->ctx;
    uv_fs_t req;
    uv_file file;
    int ret, fd;
    struct rrdeng_jf_sb *superblock;
    uv_buf_t iov;
    char path[RRDENG_PATH_MAX];

    generate_journalfilepath(datafile, path, sizeof(path));
    fd = open_file_direct_io(path, O_CREAT | O_RDWR | O_TRUNC, &file);
    if (fd < 0) {
        ++ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
        return fd;
    }
    journalfile->file = file;
    ++ctx->stats.journalfile_creations;

    ret = posix_memalign((void *)&superblock, RRDFILE_ALIGNMENT, sizeof(*superblock));
    if (unlikely(ret)) {
        fatal("DBENGINE: posix_memalign:%s", strerror(ret));
    }
    memset(superblock, 0, sizeof(*superblock));
    (void) strncpy(superblock->magic_number, RRDENG_JF_MAGIC, RRDENG_MAGIC_SZ);
    (void) strncpy(superblock->version, RRDENG_JF_VER, RRDENG_VER_SZ);

    iov = uv_buf_init((void *)superblock, sizeof(*superblock));

    ret = uv_fs_write(NULL, &req, file, &iov, 1, 0, NULL);
    if (ret < 0) {
        fatal_assert(req.result < 0);
        error("DBENGINE: uv_fs_write: %s", uv_strerror(ret));
        ++ctx->stats.io_errors;
        rrd_stat_atomic_add(&global_io_errors, 1);
    }
    uv_fs_req_cleanup(&req);
    posix_memfree(superblock);
    if (ret < 0) {
        destroy_journal_file_unsafe(journalfile, datafile);
        return ret;
    }

    journalfile->pos = sizeof(*superblock);
    ctx->stats.io_write_bytes += sizeof(*superblock);
    ++ctx->stats.io_write_requests;

    return 0;
}

static int check_journal_file_superblock(uv_file file)
{
    int ret;
    struct rrdeng_jf_sb *superblock;
    uv_buf_t iov;
    uv_fs_t req;

    ret = posix_memalign((void *)&superblock, RRDFILE_ALIGNMENT, sizeof(*superblock));
    if (unlikely(ret)) {
        fatal("DBENGINE: posix_memalign:%s", strerror(ret));
    }
    iov = uv_buf_init((void *)superblock, sizeof(*superblock));

    ret = uv_fs_read(NULL, &req, file, &iov, 1, 0, NULL);
    if (ret < 0) {
        error("DBENGINE: uv_fs_read: %s", uv_strerror(ret));
        uv_fs_req_cleanup(&req);
        goto error;
    }
    fatal_assert(req.result >= 0);
    uv_fs_req_cleanup(&req);

    if (strncmp(superblock->magic_number, RRDENG_JF_MAGIC, RRDENG_MAGIC_SZ) ||
        strncmp(superblock->version, RRDENG_JF_VER, RRDENG_VER_SZ)) {
        error("DBENGINE: File has invalid superblock.");
        ret = UV_EINVAL;
    } else {
        ret = 0;
    }
    error:
    posix_memfree(superblock);
    return ret;
}

static void restore_extent_metadata(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile, void *buf, unsigned max_size)
{
    static BITMAP256 page_error_map;
    unsigned i, count, payload_length, descr_size;
    struct rrdeng_jf_store_data *jf_metric_data;

    jf_metric_data = buf;
    count = jf_metric_data->number_of_pages;
    descr_size = sizeof(*jf_metric_data->descr) * count;
    payload_length = sizeof(*jf_metric_data) + descr_size;
    if (payload_length > max_size) {
        error("DBENGINE: corrupted transaction payload.");
        return;
    }

    time_t now_s = now_realtime_sec();
    for (i = 0; i < count ; ++i) {
        uuid_t *temp_id;
        uint8_t page_type = jf_metric_data->descr[i].type;

        if (page_type > PAGE_TYPE_MAX) {
            if (!bitmap256_get_bit(&page_error_map, page_type)) {
                error("DBENGINE: unknown page type %d encountered.", page_type);
                bitmap256_set_bit(&page_error_map, page_type, 1);
            }
            continue;
        }

        temp_id = (uuid_t *)jf_metric_data->descr[i].uuid;
        METRIC *metric = mrg_metric_get_and_acquire(main_mrg, temp_id, (Word_t) ctx);

        struct rrdeng_extent_page_descr *descr = &jf_metric_data->descr[i];
        VALIDATED_PAGE_DESCRIPTOR vd = validate_extent_page_descr(
                descr, now_s,
                (metric) ? mrg_metric_get_update_every_s(main_mrg, metric) : 0,
                false);

        if(!vd.data_on_disk_valid) {
            mrg_metric_release(main_mrg, metric);
            continue;
        }

        bool update_metric_time = true;
        if (!metric) {
            MRG_ENTRY entry = {
                    .section = (Word_t)ctx,
                    .first_time_s = vd.start_time_s,
                    .last_time_s = vd.end_time_s,
                    .latest_update_every_s = vd.update_every_s,
            };
            uuid_copy(entry.uuid, *temp_id);

            bool added;
            metric = mrg_metric_add_and_acquire(main_mrg, entry, &added);
            if(added)
                update_metric_time = false;
        }
        Word_t metric_id = mrg_metric_id(main_mrg, metric);

        if (update_metric_time)
            mrg_metric_expand_retention(main_mrg, metric, vd.start_time_s, vd.end_time_s, vd.update_every_s);

        pgc_open_add_hot_page(
                (Word_t)ctx, metric_id, vd.start_time_s, vd.end_time_s, vd.update_every_s,
                journalfile->datafile,
                jf_metric_data->extent_offset, jf_metric_data->extent_size, jf_metric_data->descr[i].page_length);

        mrg_metric_release(main_mrg, metric);
    }
}

/*
 * Replays transaction by interpreting up to max_size bytes from buf.
 * Sets id to the current transaction id or to 0 if unknown.
 * Returns size of transaction record or 0 for unknown size.
 */
static unsigned replay_transaction(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile,
                                   void *buf, uint64_t *id, unsigned max_size)
{
    unsigned payload_length, size_bytes;
    int ret;
    /* persistent structures */
    struct rrdeng_jf_transaction_header *jf_header;
    struct rrdeng_jf_transaction_trailer *jf_trailer;
    uLong crc;

    *id = 0;
    jf_header = buf;
    if (STORE_PADDING == jf_header->type) {
        debug(D_RRDENGINE, "Skipping padding.");
        return 0;
    }
    if (sizeof(*jf_header) > max_size) {
        error("DBENGINE: corrupted transaction record, skipping.");
        return 0;
    }
    *id = jf_header->id;
    payload_length = jf_header->payload_length;
    size_bytes = sizeof(*jf_header) + payload_length + sizeof(*jf_trailer);
    if (size_bytes > max_size) {
        error("DBENGINE: corrupted transaction record, skipping.");
        return 0;
    }
    jf_trailer = buf + sizeof(*jf_header) + payload_length;
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, buf, sizeof(*jf_header) + payload_length);
    ret = crc32cmp(jf_trailer->checksum, crc);
    debug(D_RRDENGINE, "Transaction %"PRIu64" was read from disk. CRC32 check: %s", *id, ret ? "FAILED" : "SUCCEEDED");
    if (unlikely(ret)) {
        error("DBENGINE: transaction %"PRIu64" was read from disk. CRC32 check: FAILED", *id);
        return size_bytes;
    }
    switch (jf_header->type) {
    case STORE_DATA:
        debug(D_RRDENGINE, "Replaying transaction %"PRIu64"", jf_header->id);
        restore_extent_metadata(ctx, journalfile, buf + sizeof(*jf_header), payload_length);
        break;
    default:
        error("DBENGINE: unknown transaction type, skipping record.");
        break;
    }

    return size_bytes;
}


#define READAHEAD_BYTES (RRDENG_BLOCK_SIZE * 256)
/*
 * Iterates journal file transactions and populates the page cache.
 * Page cache must already be initialized.
 * Returns the maximum transaction id it discovered.
 */
static uint64_t iterate_transactions(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile)
{
    uv_file file;
    uint64_t file_size;//, data_file_size;
    int ret;
    uint64_t pos, pos_i, max_id, id;
    unsigned size_bytes;
    void *buf;
    uv_buf_t iov;
    uv_fs_t req;

    file = journalfile->file;
    file_size = journalfile->pos;
    //data_file_size = journalfile->datafile->pos; TODO: utilize this?

    max_id = 1;
    bool journal_is_mmapped = (journalfile->data != NULL);
    if (unlikely(!journal_is_mmapped)) {
        ret = posix_memalign((void *)&buf, RRDFILE_ALIGNMENT, READAHEAD_BYTES);
        if (unlikely(ret))
            fatal("DBENGINE: posix_memalign:%s", strerror(ret));
    }
    else
        buf = journalfile->data +  sizeof(struct rrdeng_jf_sb);
    for (pos = sizeof(struct rrdeng_jf_sb) ; pos < file_size ; pos += READAHEAD_BYTES) {
        size_bytes = MIN(READAHEAD_BYTES, file_size - pos);
        if (unlikely(!journal_is_mmapped)) {
            iov = uv_buf_init(buf, size_bytes);
            ret = uv_fs_read(NULL, &req, file, &iov, 1, pos, NULL);
            if (ret < 0) {
                error("DBENGINE: uv_fs_read: pos=%" PRIu64 ", %s", pos, uv_strerror(ret));
                uv_fs_req_cleanup(&req);
                goto skip_file;
            }
            fatal_assert(req.result >= 0);
            uv_fs_req_cleanup(&req);
            ++ctx->stats.io_read_requests;
            ctx->stats.io_read_bytes += size_bytes;
        }

        for (pos_i = 0 ; pos_i < size_bytes ; ) {
            unsigned max_size;

            max_size = pos + size_bytes - pos_i;
            ret = replay_transaction(ctx, journalfile, buf + pos_i, &id, max_size);
            if (!ret) /* TODO: support transactions bigger than 4K */
                /* unknown transaction size, move on to the next block */
                pos_i = ALIGN_BYTES_FLOOR(pos_i + RRDENG_BLOCK_SIZE);
            else
                pos_i += ret;
            max_id = MAX(max_id, id);
        }
        if (likely(journal_is_mmapped))
            buf += size_bytes;
    }
skip_file:
    if (unlikely(!journal_is_mmapped))
        posix_memfree(buf);
    return max_id;
}

// Checks that the extent list checksum is valid
static int check_journal_v2_extent_list (void *data_start, size_t file_size)
{
    UNUSED(file_size);
    uLong crc;

    struct journal_v2_header *j2_header = (void *) data_start;
    struct journal_v2_block_trailer *journal_v2_trailer;

    journal_v2_trailer = (struct journal_v2_block_trailer *) ((uint8_t *) data_start + j2_header->extent_trailer_offset);
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (uint8_t *) data_start + j2_header->extent_offset, j2_header->extent_count * sizeof(struct journal_extent_list));
    if (unlikely(crc32cmp(journal_v2_trailer->checksum, crc))) {
        error("DBENGINE: extent list CRC32 check: FAILED");
        return 1;
    }

    return 0;
}

// Checks that the metric list (UUIDs) checksum is valid
static int check_journal_v2_metric_list(void *data_start, size_t file_size)
{
    UNUSED(file_size);
    uLong crc;

    struct journal_v2_header *j2_header = (void *) data_start;
    struct journal_v2_block_trailer *journal_v2_trailer;

    journal_v2_trailer = (struct journal_v2_block_trailer *) ((uint8_t *) data_start + j2_header->metric_trailer_offset);
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (uint8_t *) data_start + j2_header->metric_offset, j2_header->metric_count * sizeof(struct journal_metric_list));
    if (unlikely(crc32cmp(journal_v2_trailer->checksum, crc))) {
        error("DBENGINE: metric list CRC32 check: FAILED");
        return 1;
    }
    return 0;
}

//
// Return
//   0 Ok
//   1 Invalid
//   2 Force rebuild
//   3 skip

static int check_journal_v2_file(void *data_start, size_t file_size, uint32_t original_size)
{
    int rc;
    uLong crc;

    struct journal_v2_header *j2_header = (void *) data_start;
    struct journal_v2_block_trailer *journal_v2_trailer;

    if (j2_header->magic == JOURVAL_V2_REBUILD_MAGIC)
        return 2;

    if (j2_header->magic == JOURVAL_V2_SKIP_MAGIC)
        return 3;

    // Magic failure
    if (j2_header->magic != JOURVAL_V2_MAGIC)
        return 1;

    if (j2_header->total_file_size != file_size)
        return 1;

    if (original_size && j2_header->original_file_size != original_size)
        return 1;

    journal_v2_trailer = (struct journal_v2_block_trailer *) ((uint8_t *) data_start + file_size - sizeof(*journal_v2_trailer));

    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (void *) j2_header, sizeof(*j2_header));

    rc = crc32cmp(journal_v2_trailer->checksum, crc);
    if (unlikely(rc)) {
        error("DBENGINE: file CRC32 check: FAILED");
        return 1;
    }

    rc = check_journal_v2_extent_list(data_start, file_size);
    if (rc) return 1;

    rc = check_journal_v2_metric_list(data_start, file_size);
    if (rc) return 1;

    if (!db_engine_journal_check)
        return 0;

    // Verify complete UUID chain

    struct journal_metric_list *metric = (void *) (data_start + j2_header->metric_offset);

    unsigned verified = 0;
    unsigned entries;
    unsigned total_pages = 0;

    info("DBENGINE: checking %u metrics that exist in the journal", j2_header->metric_count);
    for (entries = 0; entries < j2_header->metric_count; entries++) {

        char uuid_str[UUID_STR_LEN];
        uuid_unparse_lower(metric->uuid, uuid_str);
        struct journal_page_header *metric_list_header = (void *) (data_start + metric->page_offset);
        struct journal_page_header local_metric_list_header = *metric_list_header;

        local_metric_list_header.crc = JOURVAL_V2_MAGIC;

        crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, (void *) &local_metric_list_header, sizeof(local_metric_list_header));
        rc = crc32cmp(metric_list_header->checksum, crc);

        if (!rc) {
            struct journal_v2_block_trailer *journal_trailer =
                (void *) data_start + metric->page_offset + sizeof(struct journal_page_header) + (metric_list_header->entries * sizeof(struct journal_page_list));

            crc = crc32(0L, Z_NULL, 0);
            crc = crc32(crc, (uint8_t *) metric_list_header + sizeof(struct journal_page_header), metric_list_header->entries * sizeof(struct journal_page_list));
            rc = crc32cmp(journal_trailer->checksum, crc);
            internal_error(rc, "DBENGINE: index %u : %s entries %u at offset %u verified, DATA CRC computed %lu, stored %u", entries, uuid_str, metric->entries, metric->page_offset,
                           crc, metric_list_header->crc);
            if (!rc) {
                total_pages += metric_list_header->entries;
                verified++;
            }
        }

        metric++;
        if ((uint32_t)((uint8_t *) metric - (uint8_t *) data_start) > (uint32_t) file_size) {
            info("DBENGINE: verification failed EOF reached -- total entries %u, verified %u", entries, verified);
            return 1;
        }
    }

    if (entries != verified) {
        info("DBENGINE: verification failed -- total entries %u, verified %u", entries, verified);
        return 1;
    }
    info("DBENGINE: verification succeeded -- total entries %u, verified %u (%u total pages)", entries, verified, total_pages);

    return 0;
}

int load_journal_file_v2(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile, struct rrdengine_datafile *datafile)
{
    int ret, fd;
    uint64_t file_size;
    char path[RRDENG_PATH_MAX];
    struct stat statbuf;
    uint32_t original_file_size = 0;

    generate_journalfilepath(datafile, path, sizeof(path));
    ret = stat(path, &statbuf);
    if (!ret)
        original_file_size = (uint32_t)statbuf.st_size;

    generate_journalfilepath_v2(datafile, path, sizeof(path));

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            return 1;
        ++ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
        error("DBENGINE: failed to open '%s'", path);
        return 1;
    }

    ret = fstat(fd, &statbuf);
    if (ret) {
        error("DBENGINE: failed to get file information for '%s'", path);
        close(fd);
        return 1;
    }

    file_size = (size_t)statbuf.st_size;

    if (file_size < sizeof(struct journal_v2_header)) {
        error_report("Invalid file %s. Not the expected size", path);
        close(fd);
        return 1;
    }

    usec_t start_loading = now_realtime_usec();
    uint8_t *data_start = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data_start == MAP_FAILED) {
        close(fd);
        return 1;
    }
    close(fd);

    info("DBENGINE: checking integrity of '%s'", path);
    int rc = check_journal_v2_file(data_start, file_size, original_file_size);
    if (unlikely(rc)) {
        if (rc == 2)
            error_report("File %s needs to be rebuilt", path);
        else if (rc == 3)
            error_report("File %s will be skipped", path);
        else
            error_report("File %s is invalid and it will be rebuilt", path);

        if (unlikely(munmap(data_start, file_size)))
            error("DBENGINE: failed to unmap '%s'", path);

        return rc;
    }

    struct journal_v2_header *j2_header = (void *) data_start;
    uint32_t entries = j2_header->metric_count;

    if (unlikely(!entries)) {
        if (unlikely(munmap(data_start, file_size)))
            error("DBENGINE: failed to unmap '%s'", path);

        return 1;
    }

    madvise_dontfork(data_start, file_size);
    madvise_dontdump(data_start, file_size);

    struct journal_metric_list *metric = (struct journal_metric_list *) (data_start + j2_header->metric_offset);

    // Initialize the journal file to be able to access the data
    SET_JOURNAL_DATA(journalfile, data_start);
    SET_JOURNAL_DATA_SIZE(journalfile, file_size);

    time_t header_start_time_s  = (time_t) (j2_header->start_time_ut / USEC_PER_SEC);

    time_t now_s = now_realtime_sec();
    for (size_t i=0; i < entries; i++) {
        time_t start_time_s = header_start_time_s + metric->delta_start_s;
        time_t end_time_s = header_start_time_s + metric->delta_end_s;
        time_t update_every_s = (metric->entries > 1) ? ((end_time_s - start_time_s) / (entries - 1)) : 0;
        update_metric_retention_and_granularity_by_uuid(
                ctx, &metric->uuid, start_time_s, end_time_s, update_every_s, now_s);

#ifdef NETDATA_INTERNAL_CHECKS
        struct journal_page_header *metric_list_header = (void *) (data_start + metric->page_offset);
        fatal_assert(uuid_compare(metric_list_header->uuid, metric->uuid) == 0);
        fatal_assert(metric->entries == metric_list_header->entries);
#endif
        metric++;
    }

    info("DBENGINE: journal file '%s' loaded (size:%"PRIu64") with %u metrics in %d ms", path, file_size, entries,
         (int) ((now_realtime_usec() - start_loading) / USEC_PER_MS));

    // File is OK load it
    return 0;
}

struct journal_metric_list_to_sort {
    struct jv2_metrics_info *metric_info;
};

static int journal_metric_compare (const void *item1, const void *item2)
{
    const struct jv2_metrics_info *metric1 = ((struct journal_metric_list_to_sort *) item1)->metric_info;
    const struct jv2_metrics_info *metric2 = ((struct journal_metric_list_to_sort *) item2)->metric_info;

    return uuid_compare(*(metric1->uuid), *(metric2->uuid));
}


// Write list of extents for the journalfile
void *journal_v2_write_extent_list(Pvoid_t JudyL_extents_pos, void *data)
{
    Pvoid_t *PValue;
    struct journal_extent_list *j2_extent_base = (void *) data;
    struct jv2_extents_info *ext_info;

    bool first = true;
    Word_t pos = 0;
    size_t count = 0;
    while ((PValue = JudyLFirstThenNext(JudyL_extents_pos, &pos, &first))) {
        ext_info = *PValue;
        size_t index = ext_info->index;
        j2_extent_base[index].file_index = 0;
        j2_extent_base[index].datafile_offset = ext_info->pos;
        j2_extent_base[index].datafile_size = ext_info->bytes;
        j2_extent_base[index].pages = ext_info->number_of_pages;
        count++;
    }
    return j2_extent_base + count;
}

static int verify_journal_space(struct journal_v2_header *j2_header, void *data, uint32_t bytes)
{
    if ((unsigned long)(((uint8_t *) data - (uint8_t *)  j2_header->data) + bytes) > (j2_header->total_file_size - sizeof(struct journal_v2_block_trailer)))
        return 1;

    return 0;
}

void *journal_v2_write_metric_page(struct journal_v2_header *j2_header, void *data, struct jv2_metrics_info *metric_info, uint32_t pages_offset)
{
    struct journal_metric_list *metric = (void *) data;

    if (verify_journal_space(j2_header, data, sizeof(*metric)))
        return NULL;

    uuid_copy(metric->uuid, *metric_info->uuid);
    metric->entries = metric_info->number_of_pages;
    metric->page_offset = pages_offset;
    metric->delta_start_s = (uint32_t)(metric_info->first_time_s - (time_t)(j2_header->start_time_ut / USEC_PER_SEC));
    metric->delta_end_s = (uint32_t)(metric_info->last_time_s - (time_t)(j2_header->start_time_ut / USEC_PER_SEC));

    return ++metric;
}

void *journal_v2_write_data_page_header(struct journal_v2_header *j2_header __maybe_unused, void *data, struct jv2_metrics_info *metric_info, uint32_t uuid_offset)
{
    struct journal_page_header *data_page_header = (void *) data;
    uLong crc;

    uuid_copy(data_page_header->uuid, *metric_info->uuid);
    data_page_header->entries = metric_info->number_of_pages;
    data_page_header->uuid_offset = uuid_offset;        // data header OFFSET poings to METRIC in the directory
    data_page_header->crc = JOURVAL_V2_MAGIC;
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (void *) data_page_header, sizeof(*data_page_header));
    crc32set(data_page_header->checksum, crc);
    return ++data_page_header;
}

void *journal_v2_write_data_page_trailer(struct journal_v2_header *j2_header __maybe_unused, void *data, void *page_header)
{
    struct journal_page_header *data_page_header = (void *) page_header;
    struct journal_v2_block_trailer *journal_trailer = (void *) data;
    uLong crc;

    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (uint8_t *) page_header + sizeof(struct journal_page_header), data_page_header->entries * sizeof(struct journal_page_list));
    crc32set(journal_trailer->checksum, crc);
    return ++journal_trailer;
}

void *journal_v2_write_data_page(struct journal_v2_header *j2_header, void *data, struct jv2_page_info *page_info)
{
    struct journal_page_list *data_page = data;

    if (verify_journal_space(j2_header, data, sizeof(*data_page)))
        return NULL;

    struct extent_io_data *ei = page_info->custom_data;

    data_page->delta_start_s = (uint32_t) (page_info->start_time_s - (time_t) (j2_header->start_time_ut) / USEC_PER_SEC);
    data_page->delta_end_s = (uint32_t) (page_info->end_time_s - (time_t) (j2_header->start_time_ut) / USEC_PER_SEC);
    data_page->extent_index = page_info->extent_index;

    data_page->update_every_s = page_info->update_every_s;
    data_page->page_length = (uint16_t) (ei ? ei->page_length : page_info->page_length);
    data_page->type = 0;

    return ++data_page;
}

// Must be recorded in metric_info->entries
void *journal_v2_write_descriptors(struct journal_v2_header *j2_header, void *data, struct jv2_metrics_info *metric_info)
{
    Pvoid_t *PValue;

    struct journal_page_list *data_page = (void *)data;
    // We need to write all descriptors with index metric_info->min_index_time_s, metric_info->max_index_time_s
    // that belong to this journal file
    Pvoid_t JudyL_array = metric_info->JudyL_pages_by_start_time;

    Word_t index_time = 0;
    bool first = true;
    struct jv2_page_info *page_info;
    while ((PValue = JudyLFirstThenNext(JudyL_array, &index_time, &first))) {
        page_info = *PValue;
        // Write one descriptor and return the next data page location
        data_page = journal_v2_write_data_page(j2_header, (void *)data_page, page_info);
        if (NULL == data_page)
            break;
    }
    return data_page;
}

// Migrate the journalfile pointed by datafile
// activate : make the new file active immediately
//            journafile data will be set and descriptors (if deleted) will be repopulated as needed
// startup  : if the migration is done during agent startup
//            this will allow us to optimize certain things

void do_migrate_to_v2_callback(Word_t section, unsigned datafile_fileno __maybe_unused, uint8_t type __maybe_unused,
                            Pvoid_t JudyL_metrics, Pvoid_t JudyL_extents_pos,
    size_t number_of_extents, size_t number_of_metrics, size_t number_of_pages, void *user_data)
{
    char path[RRDENG_PATH_MAX];
    Pvoid_t *PValue;
    struct rrdengine_instance *ctx = (struct rrdengine_instance *) section;
    struct rrdengine_journalfile *journalfile = (struct rrdengine_journalfile *) user_data;
    struct rrdengine_datafile *datafile = journalfile->datafile;
    time_t min_time_s = LONG_MAX;
    time_t max_time_s = 0;
    struct jv2_metrics_info *metric_info;

    generate_journalfilepath_v2(datafile, path, sizeof(path));

    info("DBENGINE: indexing file '%s': extents %zu, metrics %zu, pages %zu",
        path,
        number_of_extents,
        number_of_metrics,
        number_of_pages);

#ifdef NETDATA_INTERNAL_CHECKS
    usec_t start_loading = now_realtime_usec();
#endif

    size_t total_file_size = 0;
    total_file_size  += (sizeof(struct journal_v2_header) + JOURNAL_V2_HEADER_PADDING_SZ);

    // Extents will start here
    uint32_t extent_offset = total_file_size;
    total_file_size  += (number_of_extents * sizeof(struct journal_extent_list));

    uint32_t extent_offset_trailer = total_file_size;
    total_file_size  += sizeof(struct journal_v2_block_trailer);

    // UUID list will start here
    uint32_t metrics_offset = total_file_size;
    total_file_size  += (number_of_metrics * sizeof(struct journal_metric_list));

    // UUID list trailer
    uint32_t metric_offset_trailer = total_file_size;
    total_file_size  += sizeof(struct journal_v2_block_trailer);

    // descr @ time will start here
    uint32_t pages_offset = total_file_size;
    total_file_size  += (number_of_pages * (sizeof(struct journal_page_list) + sizeof(struct journal_page_header) + sizeof(struct journal_v2_block_trailer)));

    // File trailer
    uint32_t trailer_offset = total_file_size;
    total_file_size  += sizeof(struct journal_v2_block_trailer);

    uint8_t *data_start = netdata_mmap(path, total_file_size, MAP_SHARED, 0, false);
    uint8_t *data = data_start;

    memset(data_start, 0, extent_offset);

    // Write header
    struct journal_v2_header j2_header;
    memset(&j2_header, 0, sizeof(j2_header));

    j2_header.magic = JOURVAL_V2_MAGIC;
    j2_header.start_time_ut = 0;
    j2_header.end_time_ut = 0;
    j2_header.extent_count = number_of_extents;
    j2_header.extent_offset = extent_offset;
    j2_header.metric_count = number_of_metrics;
    j2_header.metric_offset = metrics_offset;
    j2_header.page_count = number_of_pages;
    j2_header.page_offset = pages_offset;
    j2_header.extent_trailer_offset = extent_offset_trailer;
    j2_header.metric_trailer_offset = metric_offset_trailer;
    j2_header.total_file_size = total_file_size;
    j2_header.original_file_size = (uint32_t) journalfile->pos;
    j2_header.data = data_start;                        // Used during migration

    struct journal_v2_block_trailer *journal_v2_trailer;

    data = journal_v2_write_extent_list(JudyL_extents_pos, data_start + extent_offset);
    internal_error(true, "DBENGINE: write extent list so far %llu", (now_realtime_usec() - start_loading) / USEC_PER_MS);

    fatal_assert(data == data_start + extent_offset_trailer);

    // Calculate CRC for extents
    journal_v2_trailer = (struct journal_v2_block_trailer *) (data_start + extent_offset_trailer);
    uLong crc;
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (uint8_t *) data_start + extent_offset, number_of_extents * sizeof(struct journal_extent_list));
    crc32set(journal_v2_trailer->checksum, crc);

    internal_error(true, "DBENGINE: CALCULATE CRC FOR EXTENT %llu", (now_realtime_usec() - start_loading) / USEC_PER_MS);
    // Skip the trailer, point to the metrics off
    data += sizeof(struct journal_v2_block_trailer);

    // Sanity check -- we must be at the metrics_offset
    fatal_assert(data == data_start + metrics_offset);

    // Allocate array to sort UUIDs and keep them sorted in the journal because we want to do binary search when we do lookups
    struct journal_metric_list_to_sort *uuid_list = mallocz(number_of_metrics * sizeof(struct journal_metric_list_to_sort));

    Word_t Index = 0;
    size_t count = 0;
    bool first_then_next = true;
    while ((PValue = JudyLFirstThenNext(JudyL_metrics, &Index, &first_then_next))) {
        metric_info = *PValue;

        fatal_assert(count < number_of_metrics);
        uuid_list[count++].metric_info = metric_info;
        min_time_s = MIN(min_time_s, metric_info->first_time_s);
        max_time_s = MAX(max_time_s, metric_info->last_time_s);
    }

    // Store in the header
    j2_header.start_time_ut = min_time_s * USEC_PER_SEC;
    j2_header.end_time_ut = max_time_s * USEC_PER_SEC;

    qsort(&uuid_list[0], number_of_metrics, sizeof(struct journal_metric_list_to_sort), journal_metric_compare);
    internal_error(true, "DBENGINE: traverse and qsort  UUID %llu", (now_realtime_usec() - start_loading) / USEC_PER_MS);

    uint32_t resize_file_to = total_file_size;

    for (Index = 0; Index < number_of_metrics; Index++) {
        metric_info = uuid_list[Index].metric_info;

        // Calculate current UUID offset from start of file. We will store this in the data page header
        uint32_t uuid_offset = data - data_start;

        // Write the UUID we are processing
        data  = (void *) journal_v2_write_metric_page(&j2_header, data, metric_info, pages_offset);
        if (unlikely(!data))
            break;

        // Next we will write
        //   Header
        //   Detailed entries (descr @ time)
        //   Trailer (checksum)

        // Keep the page_list_header, to be used for migration when where agent is running
        metric_info->page_list_header = pages_offset;
        // Write page header
        void *metric_page = journal_v2_write_data_page_header(&j2_header, data_start + pages_offset, metric_info, uuid_offset);

        // Start writing descr @ time
        void *page_trailer = journal_v2_write_descriptors(&j2_header, metric_page, metric_info);
        if (unlikely(!page_trailer))
            break;

        // Trailer (checksum)
        uint8_t *next_page_address = journal_v2_write_data_page_trailer(&j2_header, page_trailer, data_start + pages_offset);

        // Calculate start of the pages start for next descriptor
        pages_offset += (metric_info->number_of_pages * (sizeof(struct journal_page_list)) + sizeof(struct journal_page_header) + sizeof(struct journal_v2_block_trailer));
        // Verify we are at the right location
        if (pages_offset != (uint32_t)(next_page_address - data_start)) {
            // make sure checks fail so that we abort
            data = data_start;
            break;
        }
    }

    if (data == data_start + metric_offset_trailer) {
        internal_error(true, "DBENGINE: WRITE METRICS AND PAGES  %llu", (now_realtime_usec() - start_loading) / USEC_PER_MS);

        // Calculate CRC for metrics
        journal_v2_trailer = (struct journal_v2_block_trailer *)(data_start + metric_offset_trailer);
        crc = crc32(0L, Z_NULL, 0);
        crc =
            crc32(crc, (uint8_t *)data_start + metrics_offset, number_of_metrics * sizeof(struct journal_metric_list));
        crc32set(journal_v2_trailer->checksum, crc);
        internal_error(true, "DBENGINE: CALCULATE CRC FOR UUIDs  %llu", (now_realtime_usec() - start_loading) / USEC_PER_MS);

        // Prepare to write checksum for the file
        j2_header.data = NULL;
        journal_v2_trailer = (struct journal_v2_block_trailer *)(data_start + trailer_offset);
        crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, (void *)&j2_header, sizeof(j2_header));
        crc32set(journal_v2_trailer->checksum, crc);

        // Write header to the file
        memcpy(data_start, &j2_header, sizeof(j2_header));

        internal_error(true, "DBENGINE: FILE COMPLETED --------> %llu", (now_realtime_usec() - start_loading) / USEC_PER_MS);

        info("DBENGINE: migrated journal file '%s', file size %zu", path, total_file_size);

        SET_JOURNAL_DATA(journalfile, data_start);
        SET_JOURNAL_DATA_SIZE(journalfile, total_file_size);

        internal_error(true, "DBENGINE: ACTIVATING NEW INDEX JNL %llu", (now_realtime_usec() - start_loading) / USEC_PER_MS);
        ctx->disk_space += total_file_size;
        freez(uuid_list);
        return;
    }
    else {
        info("DBENGINE: failed to build index '%s', file will be skipped", path);
        j2_header.data = NULL;
        j2_header.magic = JOURVAL_V2_SKIP_MAGIC;
        memcpy(data_start, &j2_header, sizeof(j2_header));
        resize_file_to = sizeof(j2_header);
    }

    netdata_munmap(data_start, total_file_size);
    freez(uuid_list);

    if (likely(resize_file_to == total_file_size))
        return;

    int ret = truncate(path, (long) resize_file_to);
    if (ret < 0) {
        ctx->disk_space += total_file_size;
        ++ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
        error("DBENGINE: failed to resize file '%s'", path);
    }
    else
        ctx->disk_space += sizeof(struct journal_v2_header);
}

int load_journal_file(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile,
                      struct rrdengine_datafile *datafile)
{
    uv_fs_t req;
    uv_file file;
    int ret, fd, error;
    uint64_t file_size, max_id;
    char path[RRDENG_PATH_MAX];

    // Do not try to load the latest file (always rebuild and live migrate)
    if (datafile->fileno != ctx->last_fileno) {
        if (!load_journal_file_v2(ctx, journalfile, datafile))
            return 0;
    }

    generate_journalfilepath(datafile, path, sizeof(path));

    // If it is not the last file, open read only
    fd = open_file_direct_io(path, O_RDWR, &file);
    if (fd < 0) {
        ++ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
        return fd;
    }

    ret = check_file_properties(file, &file_size, sizeof(struct rrdeng_df_sb));
    if (ret)
        goto error;
    file_size = ALIGN_BYTES_FLOOR(file_size);

    ret = check_journal_file_superblock(file);
    if (ret) {
        info("DBENGINE: invalid journal file '%s' ; superblock check failed.", path);
        goto error;
    }
    ctx->stats.io_read_bytes += sizeof(struct rrdeng_jf_sb);
    ++ctx->stats.io_read_requests;

    journalfile->file = file;
    journalfile->pos = file_size;

    journalfile->data = netdata_mmap(path, file_size, MAP_SHARED, 0, !(datafile->fileno == ctx->last_fileno));
    info("DBENGINE: loading journal file '%s' using %s.", path, journalfile->data?"MMAP":"uv_fs_read");

    max_id = iterate_transactions(ctx, journalfile);

    ctx->commit_log.transaction_id = MAX(ctx->commit_log.transaction_id, max_id + 1);

    info("DBENGINE: journal file '%s' loaded (size:%"PRIu64").", path, file_size);
    if (likely(journalfile->data))
        netdata_munmap(journalfile->data, file_size);

    bool is_last_file = (ctx->last_fileno == journalfile->datafile->fileno);
    if (is_last_file && journalfile->datafile->pos <= rrdeng_target_data_file_size(ctx) / 3) {
        ctx->create_new_datafile_pair = false;
        return 0;
    }

    pgc_open_cache_to_journal_v2(open_cache, (Word_t) ctx, (int) datafile->fileno, ctx->page_type, do_migrate_to_v2_callback, (void *) datafile->journalfile);

    if (is_last_file)
        ctx->create_new_datafile_pair = true;

    return 0;

error:
    error = ret;
    ret = uv_fs_close(NULL, &req, file, NULL);
    if (ret < 0) {
        error("DBENGINE: uv_fs_close(%s): %s", path, uv_strerror(ret));
        ++ctx->stats.fs_errors;
        rrd_stat_atomic_add(&global_fs_errors, 1);
    }
    uv_fs_req_cleanup(&req);
    return error;
}

void init_commit_log(struct rrdengine_instance *ctx)
{
    ctx->commit_log.transaction_id = 1;
}
