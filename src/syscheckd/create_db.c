/* Copyright (C) 2015-2019, Wazuh Inc.
 * Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "shared.h"
#include "syscheck.h"
#include "syscheck_op.h"
#include "integrity_op.h"
#include "time_op.h"
#include "fim_db.h"

#ifdef UNIT_TESTING
/* Replace assert with mock_assert */
extern void mock_assert(const int result, const char* const expression,
                        const char * const file, const int line);

#undef assert
#define assert(expression) \
    mock_assert((int)(expression), #expression, __FILE__, __LINE__);
#endif

// Global variables
static int _base_line = 0;

static const char *FIM_EVENT_TYPE[] = {
    "added",
    "deleted",
    "modified"
};

static const char *FIM_EVENT_MODE[] = {
    "scheduled",
    "real-time",
    "whodata"
};

static const char *FIM_ENTRY_TYPE[] = {
    "file",
    "registry"
};

void fim_scan() {
    int it = 0;
    struct timespec start;
    struct timespec end;
    clock_t cputime_start;


    cputime_start = clock();
    gettime(&start);
    minfo(FIM_FREQUENCY_STARTED);
    fim_send_scan_info(FIM_SCAN_START);

    w_mutex_lock(&syscheck.fim_scan_mutex);

    while (syscheck.dir[it] != NULL) {
        struct fim_element *item;
        os_calloc(1, sizeof(fim_element), item);
        item->mode = FIM_SCHEDULED;
        item->index = it;
#ifndef WIN32
        if (syscheck.opts[it] & REALTIME_ACTIVE) {
            realtime_adddir(syscheck.dir[it], 0, (syscheck.opts[it] & CHECK_FOLLOW) ? 1 : 0);
        }
#endif
        fim_checker(syscheck.dir[it], item, NULL, 1);
        it++;
        os_free(item);
    }

    w_mutex_unlock(&syscheck.fim_scan_mutex);


#ifdef WIN32
        os_winreg_check();
#endif

    gettime(&end);

    if (_base_line == 0) {
        _base_line = 1;
    }

    check_deleted_files();

    minfo(FIM_FREQUENCY_ENDED);
    fim_send_scan_info(FIM_SCAN_END);

    if (isDebug()) {
        fim_print_info(start, end, cputime_start); // LCOV_EXCL_LINE
    }
}

void fim_checker(char *path, fim_element *item, whodata_evt *w_evt, int report) {
    // SQLite Development
    // fim_entry_data *saved_data;
    cJSON *json_event = NULL;
    int node;
    int depth;

#ifdef WIN_WHODATA
    if (w_evt && w_evt->scan_directory == 1) {
        if (w_update_sacl(path)) {
            mdebug1(FIM_SCAL_NOREFRESH, path);
            }
        }
#endif

    if (item->mode == FIM_SCHEDULED) {
        // If the directory have another configuration will come back
        if (node = fim_configuration_directory(path, "file"), node < 0 || item->index != node) {
            return;
        }
    } else {
        if (node = fim_configuration_directory(path, "file"), node < 0) {
            return;
        }
    }

    // We need to process every event generated by scheduled scans because we need to
    // alert about discarded events of real-time and Whodata mode
    if (item->mode != FIM_SCHEDULED && item->mode != FIM_MODE(syscheck.opts[node])) {
        return;
    }

    depth = fim_check_depth(path, node);

    if (depth > syscheck.recursion_level[node]) {
        mdebug2(FIM_MAX_RECURSION_LEVEL, depth, syscheck.recursion_level[node], path);
        return;
    }

    item->index = node;
    item->configuration = syscheck.opts[node];
    fim_entry *saved_entry = NULL;

    // Deleted file. Sending alert.
    if (w_stat(path, &(item->statbuf)) == -1) {
        if(errno != ENOENT) {
            mdebug1(FIM_STAT_FAILED, path, errno, strerror(errno));
            return;
        }

        if (item->configuration & CHECK_SEECHANGES) {
            delete_target_file(path);
        }

        w_mutex_lock(&syscheck.fim_entry_mutex);
        saved_entry = fim_db_get_path(syscheck.database, path);
        w_mutex_unlock(&syscheck.fim_entry_mutex);

        if (saved_entry) {
            json_event = fim_json_event(path, NULL, saved_entry->data, item->index, FIM_DELETE, item->mode, w_evt);
            fim_db_remove_path(syscheck.database, saved_entry, &syscheck.fim_entry_mutex, (void *) (int) 0);
            free_entry(saved_entry);
            saved_entry = NULL;
        }

        if (json_event && report) {
            char *json_formated = cJSON_PrintUnformatted(json_event);
            send_syscheck_msg(json_formated);
            os_free(json_formated);
        }
        cJSON_Delete(json_event);

        return;
    }

    if (HasFilesystem(path, syscheck.skip_fs)) {
        return;
    }

    switch(item->statbuf.st_mode & S_IFMT) {
#ifndef WIN32
    case FIM_LINK:
        // Fallthrough
#endif
    case FIM_REGULAR:
        if (fim_check_ignore(path) == 1) {
            return;
        }

        if (fim_check_restrict (path, syscheck.filerestrict[item->index]) == 1) {
            return;
        }

        if (fim_file(path, item, w_evt, report) < 0) {
            mwarn(FIM_WARN_SKIP_EVENT, path);
        }
        break;

    case FIM_DIRECTORY:
#ifndef WIN32
        if (item->configuration & REALTIME_ACTIVE) {
            realtime_adddir(path, 0, (item->configuration & CHECK_FOLLOW) ? 1 : 0);
        }
#endif
        fim_directory(path, item, w_evt, report);
        break;
    }
}


int fim_directory (char *dir, fim_element *item, whodata_evt *w_evt, int report) {
    DIR *dp;
    struct dirent *entry;
    char *f_name;
    char *s_name;
    size_t path_size;

    if (!dir) {
        merror(NULL_ERROR);
        return OS_INVALID;
    }

    // Open the directory given
    dp = opendir(dir);

    if (!dp) {
        mwarn(FIM_PATH_NOT_OPEN, dir, strerror(errno));
        return OS_INVALID;
    }

    os_calloc(PATH_MAX + 2, sizeof(char), f_name);
    while ((entry = readdir(dp)) != NULL) {
        // Ignore . and ..
        if ((strcmp(entry->d_name, ".") == 0) ||
                (strcmp(entry->d_name, "..") == 0)) {
            continue;
        }

        strncpy(f_name, dir, PATH_MAX);
        path_size = strlen(dir);
        s_name = f_name + path_size;

        // Check if the file name is already null terminated
        if (*(s_name - 1) != PATH_SEP) {
            *s_name++ = PATH_SEP;
        }
        *(s_name) = '\0';
        strncpy(s_name, entry->d_name, PATH_MAX - path_size - 2);

#ifdef WIN32
        str_lowercase(f_name);
#endif
        // Process the event related to f_name
        fim_checker(f_name, item, w_evt, report);
    }

    os_free(f_name);
    closedir(dp);
    return 0;
}


int fim_file(char *file, fim_element *item, whodata_evt *w_evt, int report) {
    fim_entry *saved = NULL;
    fim_entry_data *new = NULL;
    cJSON *json_event = NULL;
    char *json_formated;
    int alert_type;

    w_mutex_lock(&syscheck.fim_entry_mutex);

    //Get file attributes
    if (new = fim_get_data(file, item), !new) {
        mdebug1(FIM_GET_ATTRIBUTES, file);
        w_mutex_unlock(&syscheck.fim_entry_mutex);
        return 0;
    }

    if (saved = fim_db_get_path(syscheck.database, file), !saved) {
        // New entry. Insert into hash table
        alert_type = FIM_ADD;
    } else {
        // Checking for changes
        alert_type = FIM_MODIFICATION;
    }

    json_event = fim_json_event(file, saved ? saved->data : NULL, new, item->index, alert_type, item->mode, w_evt);

    if (json_event) {
        if (fim_db_insert(syscheck.database, file, new) == -1) {
            free_entry_data(new);
            free_entry(saved);
            w_mutex_unlock(&syscheck.fim_entry_mutex);
            cJSON_Delete(json_event);

            return OS_INVALID;
        }
    }

    fim_db_set_scanned(syscheck.database, file);

    w_mutex_unlock(&syscheck.fim_entry_mutex);

    if (!_base_line && item->configuration & CHECK_SEECHANGES) {
        // The first backup is created. It should return NULL.
        char *file_changed = seechanges_addfile(file);
        if (file_changed) {
            os_free(file_changed);
        }
    }

    if (json_event && _base_line && report) {
        json_formated = cJSON_PrintUnformatted(json_event);
        send_syscheck_msg(json_formated);
        os_free(json_formated);
    }

    cJSON_Delete(json_event);
    free_entry_data(new);
    free_entry(saved);

    return 0;
}


void fim_realtime_event(char *file) {

    struct stat file_stat;

    // If the file exists, generate add or modify events.
    if (w_stat(file, &file_stat) >= 0) {
        /* Need a sleep here to avoid triggering on vim
         * (and finding the file removed)
         */
        fim_rt_delay();

        fim_element item = { .mode = FIM_REALTIME };
        fim_checker(file, &item, NULL, 1);
    }
    else {
        // Otherwise, it could be a file deleted or a directory moved (or renamed).
        fim_process_missing_entry(file, FIM_REALTIME, NULL);
    }
}

void fim_whodata_event(whodata_evt * w_evt) {

    struct stat file_stat;

    // If the file exists, generate add or modify events.
    if(w_stat(w_evt->path, &file_stat) >= 0) {
        fim_rt_delay();

        fim_element item = { .mode = FIM_WHODATA };
        fim_checker(w_evt->path, &item, w_evt, 1);
    }
    // Otherwise, it could be a file deleted or a directory moved (or renamed).
    else {
        fim_process_missing_entry(w_evt->path, FIM_WHODATA, w_evt);
    }
}


void fim_process_missing_entry(char * pathname, fim_event_mode mode, whodata_evt * w_evt) {

    fim_entry *saved_data;

    // Search path in DB.
    w_mutex_lock(&syscheck.fim_entry_mutex);
    saved_data = fim_db_get_path(syscheck.database, pathname);
    w_mutex_unlock(&syscheck.fim_entry_mutex);

    // Exists, create event.
    if (saved_data) {
        fim_element item = { .mode = mode };
        fim_checker(pathname, &item, w_evt, 1);
        free_entry(saved_data);
        return;
    }

    // Since the file doesn't exist, research if it's directory and have files in DB.
    fim_tmp_file *files = NULL;
    char first_entry[PATH_MAX];
    char last_entry[PATH_MAX];

#ifdef WIN32
    snprintf(first_entry, PATH_MAX, "%s\\", pathname);
    snprintf(last_entry, PATH_MAX, "%s]", pathname);

#else
    snprintf(first_entry, PATH_MAX, "%s/", pathname);
    snprintf(last_entry, PATH_MAX, "%s0", pathname);

#endif

    w_mutex_lock(&syscheck.fim_entry_mutex);
    fim_db_get_path_range(syscheck.database, first_entry, last_entry, &files, syscheck.database_store);
    w_mutex_unlock(&syscheck.fim_entry_mutex);

    if (files && files->elements) {
        if (fim_db_process_missing_entry(syscheck.database, files, &syscheck.fim_entry_mutex,
            syscheck.database_store, mode) != FIMDB_OK) {
                merror(FIM_DB_ERROR_RM_RANGE, first_entry, last_entry);
            }
    }
}

#ifdef WIN32
int fim_registry_event(char *key, fim_entry_data *data, int pos) {

    assert(data);

    cJSON *json_event = NULL;
    fim_entry *saved;
    char *json_formated;
    int result = 1;
    int alert_type;

    w_mutex_lock(&syscheck.fim_entry_mutex);

    if (saved = fim_db_get_path(syscheck.database, key), !saved) {
        alert_type = FIM_ADD;
    } else {
        alert_type = FIM_MODIFICATION;
    }

    if ((saved && saved->data && strcmp(saved->data->hash_sha1, data->hash_sha1) != 0)
        || alert_type == FIM_ADD) {
        if (fim_db_insert(syscheck.database, key, data) == -1) {
            free_entry(saved);
            w_mutex_unlock(&syscheck.fim_entry_mutex);
            return OS_INVALID;
        }
    } else {
        fim_db_set_scanned(syscheck.database, key);
        result = 0;
    }

    w_mutex_unlock(&syscheck.fim_entry_mutex);

    json_event = fim_json_event(key, saved ? saved->data : NULL, data, pos,
                                alert_type, 0, NULL);

    if (json_event && _base_line) {
        json_formated = cJSON_PrintUnformatted(json_event);
        send_syscheck_msg(json_formated);
        os_free(json_formated);
    }
    cJSON_Delete(json_event);
    free_entry(saved);

    return result;
}
#endif

// Returns the position of the path into directories array
int fim_configuration_directory(const char *path, const char *entry) {
    char full_path[OS_SIZE_4096 + 1] = {'\0'};
    char full_entry[OS_SIZE_4096 + 1] = {'\0'};
    int it = 0;
    int top = 0;
    int match = 0;
    int position = -1;

    if (!path || *path == '\0') {
        return position;
    }

    trail_path_separator(full_path, path, sizeof(full_path));

    if (strcmp("file", entry) == 0) {
        while(syscheck.dir[it]) {
            trail_path_separator(full_entry, syscheck.dir[it], sizeof(full_entry));
            match = w_compare_str(full_entry, full_path);

            if (top < match && full_path[match - 1] == PATH_SEP) {
                position = it;
                top = match;
            }
            it++;
        }
    }
#ifdef WIN32
    else if (strcmp("registry", entry) == 0) {
        while(syscheck.registry[it].entry) {
            snprintf(full_entry, OS_SIZE_4096 + 1, "%s %s%c",
                    syscheck.registry[it].arch == ARCH_64BIT ? "[x64]" : "[x32]",
                    syscheck.registry[it].entry,
                    PATH_SEP);
            match = w_compare_str(full_entry, full_path);

            if (top < match && full_path[match - 1] == PATH_SEP) {
                position = it;
                top = match;
            }
            it++;
        }
    }
#endif

    if (position == -1) {
        mdebug2(FIM_CONFIGURATION_NOTFOUND, entry, path);
    }

    return position;
}

int fim_check_depth(char * path, int dir_position) {
    char * pos;
    int depth = -1;
    unsigned int parent_path_size;

    if (!syscheck.dir[dir_position]) {
        return -1;
    }

    parent_path_size = strlen(syscheck.dir[dir_position]);

    if (parent_path_size > strlen(path)) {
        return -1;
    }

    pos = path + parent_path_size;
    while (pos) {
        if (pos = strchr(pos, PATH_SEP), pos) {
            depth++;
        } else {
            break;
        }
        pos++;
    }

    return depth;
}


// Get data from file
fim_entry_data * fim_get_data(const char *file, fim_element *item) {
    fim_entry_data * data = NULL;

    os_calloc(1, sizeof(fim_entry_data), data);
    init_fim_data_entry(data);

    if (item->configuration & CHECK_SIZE) {
        data->size = item->statbuf.st_size;
    }

    if (item->configuration & CHECK_PERM) {
#ifdef WIN32
        int error;
        char perm[OS_SIZE_6144 + 1];

        if (error = w_get_file_permissions(file, perm, OS_SIZE_6144), error) {
            mdebug1(FIM_EXTRACT_PERM_FAIL, file, error);
            free_entry_data(data);
            return NULL;
        } else {
            data->perm = decode_win_permissions(perm);
        }
#else
        data->perm = agent_file_perm(item->statbuf.st_mode);
#endif
    }

#ifdef WIN32
    if (item->configuration & CHECK_ATTRS) {
        os_calloc(OS_SIZE_256, sizeof(char), data->attributes);
        decode_win_attributes(data->attributes, w_get_file_attrs(file));
    }
#endif

    if (item->configuration & CHECK_MTIME) {
        data->mtime = item->statbuf.st_mtime;
    }

#ifdef WIN32
    if (item->configuration & CHECK_OWNER) {
        data->user_name = get_user(file, 0, &data->uid);
    }
#else
    if (item->configuration & CHECK_OWNER) {
        char aux[OS_SIZE_64];
        snprintf(aux, OS_SIZE_64, "%u", item->statbuf.st_uid);
        os_strdup(aux, data->uid);

        data->user_name = get_user(file, item->statbuf.st_uid, NULL);
    }

    if (item->configuration & CHECK_GROUP) {
        char aux[OS_SIZE_64];
        snprintf(aux, OS_SIZE_64, "%u", item->statbuf.st_gid);
        os_strdup(aux, data->gid);

        os_strdup((char*)get_group(item->statbuf.st_gid), data->group_name);
    }
#endif

    snprintf(data->hash_md5, sizeof(os_md5), "%s", "d41d8cd98f00b204e9800998ecf8427e");
    snprintf(data->hash_sha1, sizeof(os_sha1), "%s", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    snprintf(data->hash_sha256, sizeof(os_sha256), "%s", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // The file exists and we don't have to delete it from the hash tables
    data->scanned = 1;

    // We won't calculate hash for symbolic links, empty or large files
    if ((item->statbuf.st_mode & S_IFMT) == FIM_REGULAR)
        if (item->statbuf.st_size > 0 &&
                (size_t)item->statbuf.st_size < syscheck.file_max_size &&
                ( item->configuration & CHECK_MD5SUM ||
                item->configuration & CHECK_SHA1SUM ||
                item->configuration & CHECK_SHA256SUM ) ) {
            if (OS_MD5_SHA1_SHA256_File(file,
                                        syscheck.prefilter_cmd,
                                        data->hash_md5,
                                        data->hash_sha1,
                                        data->hash_sha256,
                                        OS_BINARY,
                                        syscheck.file_max_size) < 0) {
                mdebug1(FIM_HASHES_FAIL, file);
                free_entry_data(data);
                return NULL;
        }
    }

    if (!(item->configuration & CHECK_MD5SUM)) {
        data->hash_md5[0] = '\0';
    }

    if (!(item->configuration & CHECK_SHA1SUM)) {
        data->hash_sha1[0] = '\0';
    }

    if (!(item->configuration & CHECK_SHA256SUM)) {
        data->hash_sha256[0] = '\0';
    }

    data->inode = item->statbuf.st_ino;
    data->dev = item->statbuf.st_dev;
    data->mode = item->mode;
    data->options = item->configuration;
    data->last_event = time(NULL);
    data->scanned = 1;
    // Set file entry type, registry or file
    // SQLite Development
    data->entry_type = FIM_TYPE_FILE;
    fim_get_checksum(data);

    return data;
}

void init_fim_data_entry(fim_entry_data *data) {
    data->size = 0;
    data->perm = NULL;
    data->attributes = NULL;
    data->uid = NULL;
    data->gid = NULL;
    data->user_name = NULL;
    data->group_name = NULL;
    data->mtime = 0;
    data->inode = 0;
    data->hash_md5[0] = '\0';
    data->hash_sha1[0] = '\0';
    data->hash_sha256[0] = '\0';
}

void fim_get_checksum (fim_entry_data * data) {
    char *checksum = NULL;
    int size;

    size = snprintf(0,
            0,
            "%d:%s:%s:%s:%s:%s:%s:%u:%lu:%s:%s:%s",
            data->size,
            data->perm ? data->perm : "",
            data->attributes ? data->attributes : "",
            data->uid ? data->uid : "",
            data->gid ? data->gid : "",
            data->user_name ? data->user_name : "",
            data->group_name ? data->group_name : "",
            data->mtime,
            data->inode,
            data->hash_md5,
            data->hash_sha1,
            data->hash_sha256);

    os_calloc(size + 1, sizeof(char), checksum);
    snprintf(checksum,
            size + 1,
            "%d:%s:%s:%s:%s:%s:%s:%u:%lu:%s:%s:%s",
            data->size,
            data->perm ? data->perm : "",
            data->attributes ? data->attributes : "",
            data->uid ? data->uid : "",
            data->gid ? data->gid : "",
            data->user_name ? data->user_name : "",
            data->group_name ? data->group_name : "",
            data->mtime,
            data->inode,
            data->hash_md5,
            data->hash_sha1,
            data->hash_sha256);

    OS_SHA1_Str(checksum, -1, data->checksum);
    free(checksum);
}

void check_deleted_files() {
    fim_tmp_file *file = NULL;

    w_mutex_lock(&syscheck.fim_entry_mutex);

    if (fim_db_get_not_scanned(syscheck.database, &file, syscheck.database_store) != FIMDB_OK) {
        merror(FIM_DB_ERROR_RM_NOT_SCANNED);
    }

    w_mutex_unlock(&syscheck.fim_entry_mutex);

    if (file && file->elements) {
        fim_db_delete_not_scanned(syscheck.database, file, &syscheck.fim_entry_mutex, syscheck.database_store);
    }

    w_mutex_lock(&syscheck.fim_entry_mutex);
    fim_db_set_all_unscanned(syscheck.database);
    w_mutex_unlock(&syscheck.fim_entry_mutex);
}


cJSON * fim_json_event(char * file_name, fim_entry_data * old_data, fim_entry_data * new_data, int pos, unsigned int type, fim_event_mode mode, whodata_evt * w_evt) {
    cJSON * changed_attributes = NULL;

    if (old_data != NULL) {
        changed_attributes = fim_json_compare_attrs(old_data, new_data);

        // If no such changes, do not send event.

        if (cJSON_GetArraySize(changed_attributes) == 0) {
            cJSON_Delete(changed_attributes);
            return NULL;
        }
    }

    cJSON * json_event = cJSON_CreateObject();
    cJSON_AddStringToObject(json_event, "type", "event");

    cJSON * data = cJSON_CreateObject();
    cJSON_AddItemToObject(json_event, "data", data);

    cJSON_AddStringToObject(data, "path", file_name);
    cJSON_AddStringToObject(data, "mode", FIM_EVENT_MODE[mode]);
    cJSON_AddStringToObject(data, "type", FIM_EVENT_TYPE[type]);
    cJSON_AddNumberToObject(data, "timestamp", new_data->last_event);

#ifndef WIN32
    if (old_data != NULL) {
        char** paths = NULL;

        if(paths = fim_db_get_paths_from_inode(syscheck.database, old_data->inode, old_data->dev), paths){
            if(paths[0] && paths[1]){
                cJSON *hard_links = cJSON_CreateArray();
                int i;
                for(i = 0; paths[i]; i++) {
                    if(strcmp(file_name, paths[i])) {
                        cJSON_AddItemToArray(hard_links, cJSON_CreateString(paths[i]));
                    }
                    os_free(paths[i]);
                }
                cJSON_AddItemToObject(data, "hard_links", hard_links);
            } else {
                os_free(paths[0]);
            }
            os_free(paths);
        }
    }

#endif

    cJSON_AddItemToObject(data, "attributes", fim_attributes_json(new_data));

    if (old_data) {
        cJSON_AddItemToObject(data, "changed_attributes", changed_attributes);
        cJSON_AddItemToObject(data, "old_attributes", fim_attributes_json(old_data));
    }

    char * tags = NULL;
    if (new_data->entry_type == FIM_TYPE_FILE) {
        if (w_evt) {
            cJSON_AddItemToObject(data, "audit", fim_audit_json(w_evt));
        }

        tags = syscheck.tag[pos];

        if (syscheck.opts[pos] & CHECK_SEECHANGES && type != 1) {
            char * diff = seechanges_addfile(file_name);

            if (diff != NULL) {
                cJSON_AddStringToObject(data, "content_changes", diff);
                os_free(diff);
            }
        }
    }
#ifdef WIN32
    else {
        tags = syscheck.registry[pos].tag;
    }
#endif

    if (tags != NULL) {
        cJSON_AddStringToObject(data, "tags", tags);
    }

    return json_event;
}

// Create file attribute set JSON from a FIM entry structure

cJSON * fim_attributes_json(const fim_entry_data * data) {
    cJSON * attributes = cJSON_CreateObject();

    // TODO: Read structure.
    // SQLite Development
    cJSON_AddStringToObject(attributes, "type", FIM_ENTRY_TYPE[data->entry_type]);

    if (data->options & CHECK_SIZE) {
        cJSON_AddNumberToObject(attributes, "size", data->size);
    }

    if (data->options & CHECK_PERM) {
        cJSON_AddStringToObject(attributes, "perm", data->perm);
    }

    if (data->options & CHECK_OWNER) {
        cJSON_AddStringToObject(attributes, "uid", data->uid);
    }

    if (data->options & CHECK_GROUP) {
        cJSON_AddStringToObject(attributes, "gid", data->gid);
    }

    if (data->user_name) {
        cJSON_AddStringToObject(attributes, "user_name", data->user_name);
    }

    if (data->group_name) {
        cJSON_AddStringToObject(attributes, "group_name", data->group_name);
    }

    if (data->options & CHECK_INODE) {
        cJSON_AddNumberToObject(attributes, "inode", data->inode);
    }

    if (data->options & CHECK_MTIME) {
        cJSON_AddNumberToObject(attributes, "mtime", data->mtime);
    }

    if (data->options & CHECK_MD5SUM) {
        cJSON_AddStringToObject(attributes, "hash_md5", data->hash_md5);
    }

    if (data->options & CHECK_SHA1SUM) {
        cJSON_AddStringToObject(attributes, "hash_sha1", data->hash_sha1);
    }

    if (data->options & CHECK_SHA256SUM) {
        cJSON_AddStringToObject(attributes, "hash_sha256", data->hash_sha256);
    }

#ifdef WIN32
    if (data->options & CHECK_ATTRS) {
        cJSON_AddStringToObject(attributes, "attributes", data->attributes);
    }
#endif

    if (*data->checksum) {
        cJSON_AddStringToObject(attributes, "checksum", data->checksum);
    }

    return attributes;
}

// Create file entry JSON from a FIM entry structure

cJSON * fim_entry_json(const char * path, fim_entry_data * data) {
    assert(data != NULL);
    assert(path != NULL);

    cJSON * root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddNumberToObject(root, "timestamp", data->last_event);

    cJSON * attributes = fim_attributes_json(data);
    cJSON_AddItemToObject(root, "attributes", attributes);

    return root;
}

// Create file attribute comparison JSON object

cJSON * fim_json_compare_attrs(const fim_entry_data * old_data, const fim_entry_data * new_data) {
    cJSON * changed_attributes = cJSON_CreateArray();

    if ( (old_data->options & CHECK_SIZE) && (old_data->size != new_data->size) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("size"));
    }

    if ( (old_data->options & CHECK_PERM) && strcmp(old_data->perm, new_data->perm) != 0 ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("permission"));
    }

#ifdef WIN32
    if ( (old_data->options & CHECK_ATTRS) && strcmp(old_data->attributes, new_data->attributes) != 0 ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("attributes"));
    }
#endif

    if (old_data->options & CHECK_OWNER) {
        if (old_data->uid && new_data->uid && strcmp(old_data->uid, new_data->uid) != 0) {
            cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("uid"));
        }

        if (old_data->user_name && new_data->user_name && strcmp(old_data->user_name, new_data->user_name) != 0) {
            cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("user_name"));
        }
    }

    if (old_data->options & CHECK_GROUP) {
        if (old_data->gid && new_data->gid && strcmp(old_data->gid, new_data->gid) != 0) {
            cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("gid"));
        }

        if (old_data->group_name && new_data->group_name && strcmp(old_data->group_name, new_data->group_name) != 0) {
            cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("group_name"));
        }
    }

    if ( (old_data->options & CHECK_MTIME) && (old_data->mtime != new_data->mtime) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("mtime"));
    }

#ifndef WIN32
    if ( (old_data->options & CHECK_INODE) && (old_data->inode != new_data->inode) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("inode"));
    }
#endif

    if ( (old_data->options & CHECK_MD5SUM) && (strcmp(old_data->hash_md5, new_data->hash_md5) != 0) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("md5"));
    }

    if ( (old_data->options & CHECK_SHA1SUM) && (strcmp(old_data->hash_sha1, new_data->hash_sha1) != 0) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("sha1"));
    }

    if ( (old_data->options & CHECK_SHA256SUM) && (strcmp(old_data->hash_sha256, new_data->hash_sha256) != 0) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("sha256"));
    }

    return changed_attributes;
}

// Create file audit data JSON object

cJSON * fim_audit_json(const whodata_evt * w_evt) {
    cJSON * fim_audit = cJSON_CreateObject();

    cJSON_AddStringToObject(fim_audit, "path", w_evt->path);
    cJSON_AddStringToObject(fim_audit, "user_id", w_evt->user_id);
    cJSON_AddStringToObject(fim_audit, "user_name", w_evt->user_name);
    cJSON_AddStringToObject(fim_audit, "process_name", w_evt->process_name);
    cJSON_AddNumberToObject(fim_audit, "process_id", w_evt->process_id);
#ifndef WIN32
    cJSON_AddStringToObject(fim_audit, "group_id", w_evt->group_id);
    cJSON_AddStringToObject(fim_audit, "group_name", w_evt->group_name);
    cJSON_AddStringToObject(fim_audit, "audit_uid", w_evt->audit_uid);
    cJSON_AddStringToObject(fim_audit, "audit_name", w_evt->audit_name);
    cJSON_AddStringToObject(fim_audit, "effective_uid", w_evt->effective_uid);
    cJSON_AddStringToObject(fim_audit, "effective_name", w_evt->effective_name);
    cJSON_AddNumberToObject(fim_audit, "ppid", w_evt->ppid);
#endif

    return fim_audit;
}


// Create scan info JSON event

cJSON * fim_scan_info_json(fim_scan_event event, long timestamp) {
    cJSON * root = cJSON_CreateObject();
    cJSON * data = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "type", event == FIM_SCAN_START ? "scan_start" : "scan_end");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddNumberToObject(data, "timestamp", timestamp);

    return root;
}

int fim_check_ignore (const char *file_name) {
    // Check if the file should be ignored
    if (syscheck.ignore) {
        int i = 0;
        while (syscheck.ignore[i] != NULL) {
            if (strncasecmp(syscheck.ignore[i], file_name, strlen(syscheck.ignore[i])) == 0) {
                mdebug2(FIM_IGNORE_ENTRY, "file", file_name, syscheck.ignore[i]);
                return 1;
            }
            i++;
        }
    }

    // Check in the regex entry
    if (syscheck.ignore_regex) {
        int i = 0;
        while (syscheck.ignore_regex[i] != NULL) {
            if (OSMatch_Execute(file_name, strlen(file_name), syscheck.ignore_regex[i])) {
                mdebug2(FIM_IGNORE_SREGEX, "file", file_name, syscheck.ignore_regex[i]->raw);
                return 1;
            }
            i++;
        }
    }

    return 0;
}


int fim_check_restrict (const char *file_name, OSMatch *restriction) {
    if (file_name == NULL) {
        merror(NULL_ERROR);
        return 1;
    }

    // Restrict file types
    if (restriction) {
        if (!OSMatch_Execute(file_name, strlen(file_name), restriction)) {
            mdebug2(FIM_FILE_IGNORE_RESTRICT, file_name, restriction->raw);
            return 1;
        }
    }

    return 0;
}


void free_entry_data(fim_entry_data * data) {
    if (!data) {
        return;
    }
    if (data->perm) {
        os_free(data->perm);
    }
    if (data->attributes) {
        os_free(data->attributes);
    }
    if (data->uid) {
        os_free(data->uid);
    }
    if (data->gid) {
        os_free(data->gid);
    }
    if (data->user_name) {
        os_free(data->user_name);
    }
    if (data->group_name) {
        os_free(data->group_name);
    }

    os_free(data);
}


void free_entry(fim_entry * entry) {
    if (entry) {
        os_free(entry->path);
        free_entry_data(entry->data);
        free(entry);
    }
}


void free_inode_data(fim_inode_data **data) {
    int i;

    if (*data == NULL) {
        return;
    }

    for (i = 0; i < (*data)->items; i++) {
        os_free((*data)->paths[i]);
    }
    os_free((*data)->paths);
    os_free(*data);
}

// LCOV_EXCL_START
void fim_print_info(struct timespec start, struct timespec end, clock_t cputime_start) {
    mdebug1(FIM_RUNNING_SCAN,
            time_diff(&start, &end),
            (double)(clock() - cputime_start) / CLOCKS_PER_SEC);

#ifdef WIN32
    mdebug1(FIM_ENTRIES_INFO, fim_db_get_count_entry_path(syscheck.database));
#else
    unsigned inode_items = 0;
    unsigned inode_paths = 0;

    inode_items = fim_db_get_count_entry_data(syscheck.database);
    inode_paths = fim_db_get_count_entry_path(syscheck.database);

    mdebug1(FIM_INODES_INFO, inode_items, inode_paths);
#endif

    return;
}

// Sleep during rt_delay milliseconds

void fim_rt_delay() {
#ifdef WIN32
    Sleep(syscheck.rt_delay);
#else
    struct timeval timeout = {0, syscheck.rt_delay * 1000};
    select(0, NULL, NULL, NULL, &timeout);
#endif
}

// LCOV_EXCL_STOP
