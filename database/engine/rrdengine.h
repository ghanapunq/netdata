// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_RRDENGINE_H
#define NETDATA_RRDENGINE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <lz4.h>
#include <Judy.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include "daemon/common.h"
#include "../rrd.h"
#include "rrddiskprotocol.h"
#include "rrdenginelib.h"
#include "datafile.h"
#include "journalfile.h"
#include "rrdengineapi.h"
#include "pagecache.h"
#include "metric.h"
#include "cache.h"
#include "pdc.h"

extern unsigned rrdeng_pages_per_extent;

/* Forward declarations */
struct rrdengine_instance;
struct rrdeng_cmd;

#define MAX_PAGES_PER_EXTENT (64) /* TODO: can go higher only when journal supports bigger than 4KiB transactions */

#define GET_JOURNAL_DATA(x) __atomic_load_n(&(x)->journal_data, __ATOMIC_ACQUIRE)
#define GET_JOURNAL_DATA_SIZE(x) __atomic_load_n(&(x)->journal_data_size, __ATOMIC_ACQUIRE)
#define SET_JOURNAL_DATA(x, y) __atomic_store_n(&(x)->journal_data, (y), __ATOMIC_RELEASE)
#define SET_JOURNAL_DATA_SIZE(x, y) __atomic_store_n(&(x)->journal_data_size, (y), __ATOMIC_RELEASE)

#define RRDENG_FILE_NUMBER_SCAN_TMPL "%1u-%10u"
#define RRDENG_FILE_NUMBER_PRINT_TMPL "%1.1u-%10.10u"

typedef struct page_details_control {
    struct rrdengine_instance *ctx;
    struct metric *metric;

    struct completion prep_completion;
    struct completion page_completion;   // sync between the query thread and the workers

    Pvoid_t page_list_JudyL;        // the list of page details
    unsigned completed_jobs;        // the number of jobs completed last time the query thread checked
    bool workers_should_stop;       // true when the query thread left and the workers should stop
    bool prep_done;

    SPINLOCK refcount_spinlock;     // spinlock to protect refcount
    int32_t refcount;               // the number of workers currently working on this request + 1 for the query thread
    size_t executed_with_gaps;

    time_t start_time_s;
    time_t end_time_s;
    STORAGE_PRIORITY priority;

    time_t optimal_end_time_s;

    struct {
        struct page_details_control *prev;
        struct page_details_control *next;
    } cache;
} PDC;

PDC *pdc_get(void);

typedef enum __attribute__ ((__packed__)) {
    // final status for all pages
    // if a page does not have one of these, it is considered unroutable
    PDC_PAGE_READY     = (1 << 0),                  // ready to be processed (pd->page is not null)
    PDC_PAGE_FAILED    = (1 << 1),                  // failed to be loaded (pd->page is null)
    PDC_PAGE_SKIP      = (1 << 2),                  // don't use this page, it is not good for us
    PDC_PAGE_INVALID   = (1 << 3),                  // don't use this page, it is invalid

    // other statuses for tracking issues
    PDC_PAGE_PREPROCESSED              = (1 << 4),  // used during preprocessing
    PDC_PAGE_PROCESSED                 = (1 << 5),  // processed by the query caller
    PDC_PAGE_RELEASED                  = (1 << 6),  // already released

    // data found in cache (preloaded) or on disk?
    PDC_PAGE_PRELOADED                 = (1 << 7),  // data found in memory
    PDC_PAGE_DISK_PENDING              = (1 << 8),  // data need to be loaded from disk

    // worker related statuses
    PDC_PAGE_FAILED_INVALID_EXTENT     = (1 << 9),
    PDC_PAGE_FAILED_NOT_IN_EXTENT      = (1 << 10),
    PDC_PAGE_FAILED_TO_MAP_EXTENT      = (1 << 11),
    PDC_PAGE_FAILED_TO_ACQUIRE_DATAFILE= (1 << 12),

    PDC_PAGE_EXTENT_FROM_CACHE         = (1 << 13),
    PDC_PAGE_EXTENT_FROM_DISK          = (1 << 14),

    PDC_PAGE_CANCELLED                 = (1 << 15), // the query thread had left when we try to load the page

    PDC_PAGE_SOURCE_MAIN_CACHE         = (1 << 16),
    PDC_PAGE_SOURCE_OPEN_CACHE         = (1 << 17),
    PDC_PAGE_SOURCE_JOURNAL_V2         = (1 << 18),
    PDC_PAGE_PRELOADED_PASS4           = (1 << 19),

    // datafile acquired
    PDC_PAGE_DATAFILE_ACQUIRED         = (1 << 30),
} PDC_PAGE_STATUS;

struct page_details {
    struct {
        struct rrdengine_datafile *ptr;
        uv_file file;
        unsigned fileno;

        struct {
            uint64_t pos;
            uint32_t bytes;
        } extent;
    } datafile;

    struct pgc_page *page;
    Word_t metric_id;
    time_t first_time_s;
    time_t last_time_s;
    uint32_t update_every_s;
    uint16_t page_length;
    PDC_PAGE_STATUS status;

    struct {
        struct page_details *prev;
        struct page_details *next;
    } load;

    struct {
        struct page_details *prev;
        struct page_details *next;
    } cache;
};

struct page_details *page_details_get(void);

#define pdc_page_status_check(pd, flag) (__atomic_load_n(&((pd)->status), __ATOMIC_ACQUIRE) & (flag))
#define pdc_page_status_set(pd, flag)   __atomic_or_fetch(&((pd)->status), flag, __ATOMIC_RELEASE)
#define pdc_page_status_clear(pd, flag) __atomic_and_fetch(&((od)->status), ~(flag), __ATOMIC_RELEASE)

struct jv2_extents_info {
    size_t index;
    uint64_t pos;
    unsigned bytes;
    size_t number_of_pages;
};

struct jv2_metrics_info {
    uuid_t *uuid;
    uint32_t page_list_header;
    time_t first_time_s;
    time_t last_time_s;
    size_t number_of_pages;
    Pvoid_t JudyL_pages_by_start_time;
};

struct jv2_page_info {
    time_t start_time_s;
    time_t end_time_s;
    time_t update_every_s;
    size_t page_length;
    uint32_t extent_index;
    void *custom_data;

    // private
    struct pgc_page *page;
};

typedef enum __attribute__ ((__packed__)) {
    RRDENG_CHO_UNALIGNED        = (1 << 0), // set when this metric is not page aligned according to page alignment
    RRDENG_FIRST_PAGE_ALLOCATED = (1 << 1), // set when this metric has allocated its first page
} RRDENG_COLLECT_HANDLE_OPTIONS;

struct rrdeng_collect_handle {
    struct metric *metric;
    struct pgc_page *page;
    struct pg_alignment *alignment;
    RRDENG_COLLECT_HANDLE_OPTIONS options;
    uint8_t type;
    // 2 bytes remaining here for future use
    uint32_t page_entries_max;
    uint32_t page_position;                   // keep track of the current page size, to make sure we don't exceed it
    usec_t page_end_time_ut;
    usec_t update_every_ut;
};

struct rrdeng_query_handle {
    struct metric *metric;
    struct pgc_page *page;
    struct rrdengine_instance *ctx;
    storage_number *metric_data;
    struct page_details_control *pdc;

    // the request
    time_t start_time_s;
    time_t end_time_s;
    STORAGE_PRIORITY priority;

    // internal data
    time_t now_s;
    time_t dt_s;

    unsigned position;
    unsigned entries;

    struct {
        struct rrdeng_query_handle *prev;
        struct rrdeng_query_handle *next;
    } cache;

#ifdef NETDATA_INTERNAL_CHECKS
    usec_t started_time_s;
    pid_t query_pid;
    struct rrdeng_query_handle *prev, *next;
#endif
};

struct rrdeng_query_handle *rrdeng_query_handle_get(void);
void rrdeng_query_handle_release(struct rrdeng_query_handle *handle);

enum rrdeng_opcode {
    /* can be used to return empty status or flush the command queue */
    RRDENG_OPCODE_NOOP = 0,

    RRDENG_OPCODE_EXTENT_READ,
    RRDENG_OPCODE_PREP_QUERY,
    RRDENG_OPCODE_FLUSH_PAGES,
    RRDENG_OPCODE_FLUSHED_TO_OPEN,
    RRDENG_OPCODE_FLUSH_INIT,
    RRDENG_OPCODE_EVICT_INIT,
    //RRDENG_OPCODE_DATAFILE_CREATE,
    RRDENG_OPCODE_JOURNAL_FILE_INDEX,
    RRDENG_OPCODE_DATABASE_ROTATE,
    RRDENG_OPCODE_CTX_SHUTDOWN,
    RRDENG_OPCODE_CTX_QUIESCE,

    RRDENG_OPCODE_MAX
};

// WORKERS IDS:
// RRDENG_MAX_OPCODE                     : reserved for the cleanup
// RRDENG_MAX_OPCODE + opcode            : reserved for the callbacks of each opcode
// RRDENG_MAX_OPCODE + RRDENG_MAX_OPCODE : reserved for the timer
#define RRDENG_TIMER_CB (RRDENG_OPCODE_MAX + RRDENG_OPCODE_MAX)
#define RRDENG_FLUSH_TRANSACTION_BUFFER_CB (RRDENG_TIMER_CB + 1)
#define RRDENG_OPCODES_WAITING             (RRDENG_TIMER_CB + 2)
#define RRDENG_WORKS_DISPATCHED            (RRDENG_TIMER_CB + 3)
#define RRDENG_WORKS_EXECUTING             (RRDENG_TIMER_CB + 4)

struct extent_io_data {
    unsigned fileno;
    uv_file file;
    uint64_t pos;
    unsigned bytes;
    uint16_t page_length;
};

struct extent_io_descriptor {
    struct rrdengine_instance *ctx;
    uv_fs_t uv_fs_request;
    uv_buf_t iov;
    uv_file file;
    void *buf;
    struct wal *wal;
    uint64_t pos;
    unsigned bytes;
    struct completion *completion;
    unsigned descr_count;
    struct page_descr_with_data *descr_array[MAX_PAGES_PER_EXTENT];
    struct rrdengine_datafile *datafile;
    struct extent_io_descriptor *next; /* multiple requests to be served by the same cached extent */

    struct {
        struct extent_io_descriptor *prev;
        struct extent_io_descriptor *next;
    } cache;
};

struct generic_io_descriptor {
    struct rrdengine_instance *ctx;
    uv_fs_t req;
    uv_buf_t iov;
    void *buf;
    void *data;
    uint64_t pos;
    unsigned bytes;
    struct completion *completion;
};

typedef struct wal {
    uint64_t transaction_id;
    void *buf;
    size_t size;
    size_t buf_size;
    struct generic_io_descriptor io_descr;

    struct {
        struct wal *prev;
        struct wal *next;
    } cache;
} WAL;

WAL *wal_get(struct rrdengine_instance *ctx, unsigned size);
void wal_release(WAL *wal);

struct rrdengine_worker_config {
    bool now_deleting_files;
    bool migration_to_v2_running;

    struct {
        // non-zero until we commit data to disk (both datafile and journal file)
        unsigned extents_currently_being_flushed;
    } atomics;
};

/*
 * Debug statistics not used by code logic.
 * They only describe operations since DB engine instance load time.
 */
struct rrdengine_statistics {
    rrdeng_stats_t metric_API_producers;
    rrdeng_stats_t metric_API_consumers;
    rrdeng_stats_t pg_cache_insertions;
    rrdeng_stats_t pg_cache_deletions;
    rrdeng_stats_t pg_cache_hits;
    rrdeng_stats_t pg_cache_misses;
    rrdeng_stats_t pg_cache_backfills;
    rrdeng_stats_t pg_cache_evictions;
    rrdeng_stats_t before_decompress_bytes;
    rrdeng_stats_t after_decompress_bytes;
    rrdeng_stats_t before_compress_bytes;
    rrdeng_stats_t after_compress_bytes;
    rrdeng_stats_t io_write_bytes;
    rrdeng_stats_t io_write_requests;
    rrdeng_stats_t io_read_bytes;
    rrdeng_stats_t io_read_requests;
    rrdeng_stats_t io_write_extent_bytes;
    rrdeng_stats_t io_write_extents;
    rrdeng_stats_t io_read_extent_bytes;
    rrdeng_stats_t io_read_extents;
    rrdeng_stats_t datafile_creations;
    rrdeng_stats_t datafile_deletions;
    rrdeng_stats_t journalfile_creations;
    rrdeng_stats_t journalfile_deletions;
    rrdeng_stats_t page_cache_descriptors;
    rrdeng_stats_t io_errors;
    rrdeng_stats_t fs_errors;
    rrdeng_stats_t pg_cache_over_half_dirty_events;
    rrdeng_stats_t flushing_pressure_page_deletions;
};

/* I/O errors global counter */
extern rrdeng_stats_t global_io_errors;
/* File-System errors global counter */
extern rrdeng_stats_t global_fs_errors;
/* number of File-Descriptors that have been reserved by dbengine */
extern rrdeng_stats_t rrdeng_reserved_file_descriptors;
/* inability to flush global counters */
extern rrdeng_stats_t global_pg_cache_over_half_dirty_events;
extern rrdeng_stats_t global_flushing_pressure_page_deletions; /* number of deleted pages */

#define NO_QUIESCE  (0) /* initial state when all operations function normally */
#define SET_QUIESCE (1) /* set it before shutting down the instance, quiesce long running operations */
#define QUIESCED    (2) /* is set after all threads have finished running */

struct rrdengine_instance {
    struct rrdengine_worker_config worker_config;
    struct completion rrdengine_completion;
    bool journal_initialization;
    uint8_t global_compress_alg;
    struct transaction_commit_log commit_log;
    struct rrdengine_datafile_list datafiles;
    RRDHOST *host; /* the legacy host, or NULL for multi-host DB */
    char dbfiles_path[FILENAME_MAX + 1];
    char machine_guid[GUID_LEN + 1]; /* the unique ID of the corresponding host, or localhost for multihost DB */
    uint64_t disk_space;
    uint64_t max_disk_space;
    int tier;
    unsigned last_fileno; /* newest index of datafile and journalfile */
    unsigned last_flush_fileno;
    unsigned long metric_API_max_producers;

    bool create_new_datafile_pair;
    uint8_t quiesce;   /* set to SET_QUIESCE before shutdown of the engine */
    uint8_t page_type; /* Default page type for this context */

    struct completion quiesce_completion;

    size_t inflight_queries;
    struct rrdengine_statistics stats;
};

#define ctx_is_available_for_queries(ctx) (__atomic_load_n(&(ctx)->quiesce, __ATOMIC_RELAXED) == NO_QUIESCE)

void *dbengine_page_alloc(struct rrdengine_instance *ctx, size_t size);
void dbengine_page_free(void *page);

int init_rrd_files(struct rrdengine_instance *ctx);
void finalize_rrd_files(struct rrdengine_instance *ctx);
bool rrdeng_dbengine_spawn(struct rrdengine_instance *ctx);
void dbengine_event_loop(void *arg);
typedef void (*enqueue_callback_t)(struct rrdeng_cmd *cmd);
typedef void (*dequeue_callback_t)(struct rrdeng_cmd *cmd);

void rrdeng_enqueue_epdl_cmd(struct rrdeng_cmd *cmd);
void rrdeng_dequeue_epdl_cmd(struct rrdeng_cmd *cmd);

typedef struct rrdeng_cmd *(*requeue_callback_t)(void *data);
void rrdeng_req_cmd(requeue_callback_t get_cmd_cb, void *data, STORAGE_PRIORITY priority);

void rrdeng_enq_cmd(struct rrdengine_instance *ctx, enum rrdeng_opcode opcode, void *data,
                struct completion *completion, enum storage_priority priority,
                enqueue_callback_t enqueue_cb, dequeue_callback_t dequeue_cb);

void pdc_route_asynchronously(struct rrdengine_instance *ctx, struct page_details_control *pdc);
void pdc_route_synchronously(struct rrdengine_instance *ctx, struct page_details_control *pdc);

void pdc_acquire(PDC *pdc);
bool pdc_release_and_destroy_if_unreferenced(PDC *pdc, bool worker, bool router);

unsigned rrdeng_target_data_file_size(struct rrdengine_instance *ctx);

struct page_descr_with_data *page_descriptor_get(void);

typedef struct validated_page_descriptor {
    time_t start_time_s;
    time_t end_time_s;
    time_t update_every_s;
    size_t page_length;
    size_t point_size;
    size_t entries;
    uint8_t type;
    bool data_on_disk_valid;
} VALIDATED_PAGE_DESCRIPTOR;

#define page_entries_by_time(start_time_s, end_time_s, update_every_s) \
        ((update_every_s) ? (((end_time_s) - ((start_time_s) - (update_every_s))) / (update_every_s)) : 1)

#define page_entries_by_size(page_length_in_bytes, point_size_in_bytes) \
        ((page_length_in_bytes) / (point_size_in_bytes))

VALIDATED_PAGE_DESCRIPTOR validate_extent_page_descr(const struct rrdeng_extent_page_descr *descr, time_t now_s, time_t overwrite_zero_update_every_s, bool have_read_error);

#endif /* NETDATA_RRDENGINE_H */
