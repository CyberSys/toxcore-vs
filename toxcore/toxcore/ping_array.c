/*
 * Implementation of an efficient array to store that we pinged something.
 */

/*
 * Copyright � 2016-2017 The TokTok team.
 * Copyright � 2014 Tox project.
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ping_array.h"

#include "crypto_core.h"
#include "util.h"


typedef struct {
    void *data;
    uint32_t length;
    uint64_t time;
    uint64_t ping_id;
} Ping_Array_Entry;

struct Ping_Array {
    Ping_Array_Entry *entries;

    uint32_t last_deleted; /* number representing the next entry to be deleted. */
    uint32_t last_added; /* number representing the last entry to be added. */
    uint32_t total_size; /* The length of entries */
    uint32_t timeout; /* The timeout after which entries are cleared. */
};

/* Initialize a Ping_Array.
 * size represents the total size of the array and should be a power of 2.
 * timeout represents the maximum timeout in seconds for the entry.
 *
 * return 0 on success.
 * return -1 on failure.
 */
Ping_Array *ping_array_new(uint32_t size, uint32_t timeout)
{
    if (size == 0 || timeout == 0) {
        return nullptr;
    }

    Ping_Array *empty_array = (Ping_Array *)calloc(1, sizeof(Ping_Array));

    if (empty_array == nullptr) {
        return nullptr;
    }

    empty_array->entries = (Ping_Array_Entry *)calloc(size, sizeof(Ping_Array_Entry));

    if (empty_array->entries == nullptr) {
        free(empty_array);
        return nullptr;
    }

    empty_array->last_deleted = empty_array->last_added = 0;
    empty_array->total_size = size;
    empty_array->timeout = timeout;
    return empty_array;
}

static void clear_entry(Ping_Array *array, uint32_t index)
{
    free(array->entries[index].data);
    array->entries[index].data = nullptr;
    array->entries[index].length =
        array->entries[index].time =
            array->entries[index].ping_id = 0;
}

/* Free all the allocated memory in a Ping_Array.
 */
void ping_array_kill(Ping_Array *array)
{
    while (array->last_deleted != array->last_added) {
        uint32_t index = array->last_deleted % array->total_size;
        clear_entry(array, index);
        ++array->last_deleted;
    }

    free(array->entries);
    free(array);
}

/* Clear timed out entries.
 */
static void ping_array_clear_timedout(Ping_Array *array)
{
    while (array->last_deleted != array->last_added) {
        uint32_t index = array->last_deleted % array->total_size;

        if (!is_timeout(array->entries[index].time, array->timeout)) {
            break;
        }

        clear_entry(array, index);
        ++array->last_deleted;
    }
}

/* Add a data with length to the Ping_Array list and return a ping_id.
 *
 * return ping_id on success.
 * return 0 on failure.
 */
uint64_t ping_array_add(Ping_Array *array, const uint8_t *data, uint32_t length)
{
    ping_array_clear_timedout(array);
    uint32_t index = array->last_added % array->total_size;

    if (array->entries[index].data != nullptr) {
        array->last_deleted = array->last_added - array->total_size;
        clear_entry(array, index);
    }

    array->entries[index].data = malloc(length);

    if (array->entries[index].data == nullptr) {
        return 0;
    }

    memcpy(array->entries[index].data, data, length);
    array->entries[index].length = length;
    array->entries[index].time = unix_time();
    ++array->last_added;
    uint64_t ping_id = random_u64();
    ping_id /= array->total_size;
    ping_id *= array->total_size;
    ping_id += index;

    if (ping_id == 0) {
        ping_id += array->total_size;
    }

    array->entries[index].ping_id = ping_id;
    return ping_id;
}


/* Check if ping_id is valid and not timed out.
 *
 * On success, copies the data into data of length,
 *
 * return length of data copied on success.
 * return -1 on failure.
 */
int32_t ping_array_check(Ping_Array *array, uint8_t *data, size_t length, uint64_t ping_id)
{
    if (ping_id == 0) {
        return -1;
    }

    uint32_t index = ping_id % array->total_size;

    if (array->entries[index].ping_id != ping_id) {
        return -1;
    }

    if (is_timeout(array->entries[index].time, array->timeout)) {
        return -1;
    }

    if (array->entries[index].length > length) {
        return -1;
    }

    if (array->entries[index].data == nullptr) {
        return -1;
    }

    memcpy(data, array->entries[index].data, array->entries[index].length);
    uint32_t len = array->entries[index].length;
    clear_entry(array, index);
    return len;
}
