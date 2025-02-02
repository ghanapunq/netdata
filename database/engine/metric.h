#ifndef DBENGINE_METRIC_H
#define DBENGINE_METRIC_H

#include "../rrd.h"

typedef struct metric METRIC;
typedef struct mrg MRG;

typedef struct mrg_entry {
    uuid_t uuid;
    Word_t section;
    time_t first_time_s;
    time_t last_time_s;
    uint32_t latest_update_every_s;
} MRG_ENTRY;

struct mrg_statistics {
    size_t entries;
    size_t size;                // memory without indexing
    size_t additions;
    size_t additions_duplicate;
    size_t deletions;
    size_t delete_misses;
    size_t search_hits;
    size_t search_misses;
    size_t pointer_validation_hits;
    size_t pointer_validation_misses;
};

MRG *mrg_create(void);
void mrg_destroy(MRG *mrg);

METRIC *mrg_metric_dup(MRG *mrg, METRIC *metric);
void mrg_metric_release(MRG *mrg, METRIC *metric);

METRIC *mrg_metric_add_and_acquire(MRG *mrg, MRG_ENTRY entry, bool *ret);
METRIC *mrg_metric_get_and_acquire(MRG *mrg, uuid_t *uuid, Word_t section);
bool mrg_metric_release_and_delete(MRG *mrg, METRIC *metric);

Word_t mrg_metric_id(MRG *mrg, METRIC *metric);
uuid_t *mrg_metric_uuid(MRG *mrg, METRIC *metric);
Word_t mrg_metric_section(MRG *mrg, METRIC *metric);

bool mrg_metric_set_first_time_s(MRG *mrg, METRIC *metric, time_t first_time_s);
bool mrg_metric_set_first_time_s_if_zero(MRG *mrg, METRIC *metric, time_t first_time_s);
time_t mrg_metric_get_first_time_s(MRG *mrg, METRIC *metric);
void mrg_metric_expand_retention(MRG *mrg __maybe_unused, METRIC *metric, time_t first_time_s, time_t last_time_s, time_t update_every_s);

bool mrg_metric_set_clean_latest_time_s(MRG *mrg, METRIC *metric, time_t latest_time_s);
bool mrg_metric_set_hot_latest_time_s(MRG *mrg, METRIC *metric, time_t latest_time_s);
time_t mrg_metric_get_latest_time_s(MRG *mrg, METRIC *metric);

bool mrg_metric_set_update_every(MRG *mrg, METRIC *metric, time_t update_every_s);
time_t mrg_metric_get_update_every_s(MRG *mrg, METRIC *metric);

bool mrg_metric_set_update_every_s_if_zero(MRG *mrg, METRIC *metric, time_t update_every_s);

struct mrg_statistics mrg_get_statistics(MRG *mrg);

#endif // DBENGINE_METRIC_H
